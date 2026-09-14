#include "mca_diag.h"
#include "mca_config.h"
#include "mca_dsp.h"
#include "adc_cap.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

static const char *TAG = "mca_diag";

_Static_assert(DIAG_SCOPE_PRE < DIAG_SCOPE_LEN, "предыстория длиннее окна");
_Static_assert(DIAG_TRIG_SPAN < DIAG_SCOPE_PRE, "перепад длиннее предыстории");

static mca_diag_t s_d;
static volatile bool s_enabled;

/* СПИН-БЛОКИРОВКА, а не мьютекс: под ней только несколько слов
 * (счётчики, указатель и номер снимка) - доли микросекунды.
 *
 * Раньше был мьютекс, и веб держал его, пока копировал снимок
 * осциллограммы - до 32 КБ из PSRAM, около миллисекунды, а веб-задачу
 * в это время ещё и вытесняет WiFi. Задача обработки берёт эту же
 * блокировку на КАЖДОМ чанке, а запас буферов захвата на 16 МГц - 2 мс.
 * Итог: потерянные чанки, за каждым - сброс сборки окна осциллографа,
 * и обновление падало с 11-12 до 4 раз в секунду. Теперь снимок
 * копируется без блокировки (см. mca_diag_get_scope). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)

static uint64_t s_prev_samples;
static int64_t  s_next_analyze_us;   /* когда разрешён следующий разбор */

/* ---- ОСЦИЛЛОГРАММА С СИНХРОНИЗАЦИЕЙ ПО ФРОНТУ ----
 *
 * Как у настоящего осциллографа: момент срабатывания всегда стоит на
 * одном месте окна - после DIAG_SCOPE_PRE отсчётов предыстории, - и
 * импульс не прыгает по экрану. Прежде снимок отдавался целиком, как
 * только где-то в окне был перепад, и импульс оказывался где попало.
 *
 * Окно собирается из ПОДРЯД идущих чанков. Раньше ограничение "10 раз
 * в секунду" стояло перед копированием каждого куска, и окно
 * склеивалось из кусков, взятых через 100 мс: ось времени рвалась.
 *
 * Условие срабатывания - ПЕРЕСЕЧЕНИЕ уровня перепадом вверх за
 * DIAG_TRIG_SPAN отсчётов. От базовой линии не зависит и ловит фронт
 * даже на хвосте предыдущего импульса. Именно пересечение, а не
 * "выше уровня": взведясь посреди фронта, иначе сработали бы на
 * первом же отсчёте, и импульс встал бы со сдвигом. */
#define AUTO_WAIT_US   150000   /* авто: столько ждём фронт, потом пуск */
/* Не чаще 20 снимков в секунду. Страница опрашивает раз в 100 мс, и при
 * том же периоде здесь из-за сдвига фаз ей то и дело доставался бы
 * прежний снимок - картинка дёргалась бы. */
#define SNAP_PERIOD_US 50000

static uint16_t *s_scope;          /* опубликованный снимок                */
static uint16_t *s_build;          /* собирается                           */
static size_t   s_scope_len;
static int32_t  s_scope_trig = -1; /* индекс срабатывания, -1 = без синхр. */
static int32_t  s_scope_rise;
static int64_t  s_scope_time;
static volatile bool s_scope_rd;   /* веб копирует опубликованный снимок   */

/* Предыстория НЕ копится отдельным массивом: держим указатели на два
 * предыдущих чанка прямо в буферах захвата. Раньше на каждый чанк
 * приходилось до 6 КБ копий (memmove + memcpy), а при 16 МГц весь
 * бюджет на чанк - 128 мкс. Буферы захвата живут 16 чанков (2 мс), а
 * предыстория копируется сразу при срабатывании, так что затереть её
 * не успевают. */
static const uint16_t *s_prv[2];
static size_t   s_prv_n[2];
static int      s_prv_i;          /* куда писать следующий */

static enum { SC_IDLE, SC_ARMED, SC_POST } s_sc;
static size_t   s_fill;
static int32_t  s_cap_trig, s_cap_rise;
static int64_t  s_arm_us;
static int64_t  s_snap_next_us;
static int32_t  s_trig_level = 30;

void mca_diag_init(void)
{
    memset(&s_d, 0, sizeof(s_d));

    /* Оба буфера окна - во ВНУТРЕННЕЙ памяти (по 16 КБ). В PSRAM их
     * запись задачей обработки и чтение веб-сервером шли по внешней
     * шине вместе с выборкой кода WiFi из флеша, и обновление
     * осциллограммы замирало тем сильнее, чем длиннее развёртка. */
    s_scope = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_INTERNAL);
    s_build = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_INTERNAL);
    if (!s_scope || !s_build) {
        ESP_LOGW(TAG, "окно осциллографа не влезло во внутреннюю память, "
                      "беру PSRAM - обновление будет медленнее");
        free(s_scope);
        free(s_build);
        s_scope = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_SPIRAM);
        s_build = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_SPIRAM);
    }
    s_enabled = false;
}

void mca_diag_set_enabled(bool en)
{
    s_enabled = en;
    if (!en) {
        LOCK();
        s_d.stats_valid = false;
        UNLOCK();
    }
    ESP_LOGI(TAG, "диагностика %s", en ? "включена" : "выключена");
}

bool mca_diag_get_enabled(void) { return s_enabled; }

/* Запомнить чанк как предысторию для следующих (только указатель). */
static void prv_push(const uint16_t *d, size_t n)
{
    s_prv[s_prv_i]   = d;
    s_prv_n[s_prv_i] = n;
    s_prv_i ^= 1;
}

#define PRV_NEW (s_prv_i ^ 1)     /* более свежий предыдущий чанк */
#define PRV_OLD (s_prv_i)         /* и тот, что перед ним         */

/* Перепад за DIAG_TRIG_SPAN отсчётов в точке j текущего чанка. Отсчёт
 * j-K берётся из предыдущего чанка, если j меньше K. INT32_MIN -
 * предыстории не хватает. */
static inline int32_t trig_diff(const uint16_t *d, size_t j, size_t K,
                                int32_t sg, const uint16_t *pv, size_t pvn)
{
    int32_t ref;
    if (j >= K)                 ref = d[j - K] & MCA_DATA_MASK;
    else if (pvn >= K - j)      ref = pv[pvn - (K - j)] & MCA_DATA_MASK;
    else                        return INT32_MIN;
    return sg * ((int32_t)(d[j] & MCA_DATA_MASK) - ref);
}

/* Дописать в окно сколько влезет; true - окно заполнено. */
static bool cap_take(const uint16_t *d, size_t n)
{
    size_t take = DIAG_SCOPE_LEN - s_fill;
    if (take > n) take = n;
    memcpy(s_build + s_fill, d, take * 2);
    s_fill += take;
    return s_fill >= DIAG_SCOPE_LEN;
}

/* Начать окно так, чтобы отсчёт d[j] встал после предыстории.
 * Предыстория собирается здесь, ОДИН раз на срабатывание: сначала из
 * более старого предыдущего чанка, затем из свежего, затем из начала
 * текущего. */
static bool cap_start(const uint16_t *d, size_t n, size_t j)
{
    size_t need = DIAG_SCOPE_PRE;
    const size_t cur = j < need ? j : need;
    need -= cur;

    size_t n_new = 0, n_old = 0;
    if (need) {
        n_new = s_prv_n[PRV_NEW] < need ? s_prv_n[PRV_NEW] : need;
        need -= n_new;
    }
    if (need) {
        n_old = s_prv_n[PRV_OLD] < need ? s_prv_n[PRV_OLD] : need;
        need -= n_old;
    }

    size_t pos = 0;
    if (n_old) {
        memcpy(s_build, s_prv[PRV_OLD] + s_prv_n[PRV_OLD] - n_old, n_old * 2);
        pos += n_old;
    }
    if (n_new) {
        memcpy(s_build + pos, s_prv[PRV_NEW] + s_prv_n[PRV_NEW] - n_new,
               n_new * 2);
        pos += n_new;
    }
    if (cur) {
        memcpy(s_build + pos, d + j - cur, cur * 2);
        pos += cur;
    }
    s_fill     = pos;
    s_cap_trig = (int32_t)pos;
    return cap_take(d + j, n - j);
}

static void cap_publish(int64_t now)
{
    /* Буферы меняем местами, а не копируем. Но пока веб копирует
     * опубликованный снимок, обмен НЕ делаем: иначе его буфер уйдёт под
     * сборку и начнёт перезаписываться. Такой кадр просто пропускаем -
     * снимки выходят 20 раз в секунду, страница забирает 10, потеря
     * незаметна.
     *
     * Прошлый вариант (копировать без блокировки и повторять, если
     * снимок подменили) давал биение: период публикации 50 мс и время
     * копирования 32 КБ сходились по фазе, повторы упирались в предел,
     * страница получала пустой ответ - обновление шло волнами примерно
     * по две секунды. */
    LOCK();
    if (!s_scope_rd) {
        uint16_t *t = s_scope;
        s_scope = s_build;
        s_build = t;
        s_scope_len  = DIAG_SCOPE_LEN;
        s_scope_trig = s_cap_trig;
        s_scope_rise = s_cap_rise;
        s_scope_time = now;
    }
    UNLOCK();
    s_sc = SC_IDLE;
    s_snap_next_us = now + SNAP_PERIOD_US;
}

/* Хватит ли предыстории из буферов захвата, пока их не перезаписал DMA.
 * Кольцо буферов захвата крутится само, без разрешения задачи
 * обработки: буфер предпредыдущего чанка снова пишется, когда в очереди
 * скопилось CAP_N_CHUNKS-2 чанков. С запасом: при очереди глубже
 * CAP_N_CHUNKS-4 срабатывание пропускаем (дождёмся следующего фронта),
 * а в свободном пуске берём окно без предыстории. */
static bool prv_safe(void)
{
    return adc_cap_queue_depth() <= CAP_N_CHUNKS - 4;
}

static void scope_step(const uint16_t *d, size_t n, diag_scope_mode_t sm,
                       int64_t now)
{
    if (s_sc == SC_IDLE) {
        if (now < s_snap_next_us) return;
        /* Взводимся, а искать фронт начинаем со СЛЕДУЮЩЕГО чанка:
         * для него этот станет предысторией. */
        s_sc = SC_ARMED;
        s_arm_us = now;
        return;
    }

    if (s_sc == SC_POST) {
        if (cap_take(d, n)) cap_publish(now);
        return;
    }

    /* Без предыдущего чанка фронт не ищем: момент синхронизации встал бы
     * не на своё место, да и перепад на первых отсчётах не посчитать. */
    if (!s_prv_n[PRV_NEW]) return;

    /* SC_ARMED: ищем пересечение уровня */
    const int32_t lvl = s_trig_level;
    const size_t  K   = DIAG_TRIG_SPAN;
    /* импульсы вниз - синхронизация по фронту вниз */
    const int32_t sg  = mca_dsp_polarity_neg() ? -1 : 1;
    const uint16_t *pv = s_prv[PRV_NEW];
    const size_t    pvn = s_prv_n[PRV_NEW];

    /* ПОИСК ШАГОМ TRIG_STEP, потом уточнение.
     *
     * Полный перебор 2046 отсчётов на каждом чанке при 16 МГц съедал
     * почти весь бюджет 128 мкс: очередь чанков стояла забитой (15 из
     * 16 по замеру), и любой всплеск давал потерю. У настоящего фронта
     * перепад за 8 отсчётов держится выше уровня несколько отсчётов
     * подряд, поэтому грубый проход его не пропускает. Точное место
     * ПЕРВОГО пересечения ищется поштучно внутри найденного шага, так
     * что момент синхронизации остаётся тем же. Пропустить можно только
     * выброс короче четырёх отсчётов - это шум, не импульс. */
#define TRIG_STEP 4
    int32_t r = 0;
    size_t  j = 0;
    bool    hit = false;

    for (size_t c = 0; c < n && !hit; c += TRIG_STEP) {
        const int32_t rc = trig_diff(d, c, K, sg, pv, pvn);
        if (rc == INT32_MIN || rc < lvl) continue;

        /* пересечение не раньше, чем на предыдущем шаге */
        const size_t lo = (c >= TRIG_STEP) ? c - TRIG_STEP + 1 : 0;
        int32_t rp;
        if (lo > 0) {
            rp = trig_diff(d, lo - 1, K, sg, pv, pvn);
            if (rp == INT32_MIN) rp = INT32_MAX;
        } else if (pvn > K) {
            rp = sg * ((int32_t)(pv[pvn - 1] & MCA_DATA_MASK)
                       - (int32_t)(pv[pvn - 1 - K] & MCA_DATA_MASK));
        } else {
            rp = INT32_MAX;      /* судить не по чему - не считаем фронтом */
        }
        for (size_t x = lo; x <= c; x++) {
            const int32_t rx = trig_diff(d, x, K, sg, pv, pvn);
            if (rx == INT32_MIN) { rp = INT32_MAX; continue; }
            if (rx >= lvl && rp < lvl) { j = x; r = rx; hit = true; break; }
            rp = rx;
        }
    }

    if (hit && prv_safe()) {
        s_cap_rise = r;
        if (cap_start(d, n, j)) cap_publish(now);
        else                    s_sc = SC_POST;
        return;
    }

    if (sm == DIAG_SCOPE_AUTO && now - s_arm_us > AUTO_WAIT_US) {
        /* фронта нет - свободный пуск, чтобы экран не замер */
        s_cap_rise = 0;
        if (!prv_safe()) s_prv_n[0] = s_prv_n[1] = 0;
        bool full = cap_start(d, n, 0);
        s_cap_trig = -1;
        if (full) cap_publish(now);
        else      s_sc = SC_POST;
    }
}

static void scope_feed(const uint16_t *d, size_t n, diag_scope_mode_t sm,
                       int64_t now)
{
    if (!s_scope || !s_build) return;
    scope_step(d, n, sm, now);
    /* Этот чанк - предыстория для следующих. Запоминается всегда, в
     * любом состоянии: это два присваивания. */
    prv_push(d, n);
}

/* Предыстория без разрыва: после потери чанка или выхода из режима
 * осциллографа старые указатели ведут в уже перезаписанные буферы. */
static void prv_reset(void)
{
    s_prv_n[0] = s_prv_n[1] = 0;
}

void mca_diag_flush(void)
{
    s_sc = SC_IDLE;
    prv_reset();
}

void mca_diag_feed(const uint16_t *data, size_t n, diag_scope_mode_t sm)
{
    /* Только счётчики - это действительно дёшево. */
    LOCK();
    s_d.chunks_total++;
    s_d.samples_total += n;
    UNLOCK();

    if (sm == DIAG_SCOPE_OFF) {
        s_sc = SC_IDLE;          /* вернёмся в осциллограф - соберём заново */
        prv_reset();
        if (!s_enabled) return;  /* в режиме спектра дальше ничего не нужно */
    }

    int64_t now = esp_timer_get_time();
    if (sm != DIAG_SCOPE_OFF) scope_feed(data, n, sm, now);

    if (!s_enabled) return;

    /* Побитовая статистика - раз в секунду, по одному чанку. */
    if (now < s_next_analyze_us) return;
    s_next_analyze_us = now + 1000000;

    uint32_t toggles[DIAG_NBITS] = { 0 };
    uint16_t and_acc = 0x1FFF, or_acc = 0;
    uint16_t vmin = 0xFFFF, vmax = 0;
    uint64_t sum = 0;
    uint32_t otr = 0;

    uint16_t prev = data[0] & 0x1FFF;
    for (size_t i = 0; i < n; i++) {
        uint16_t raw = data[i] & 0x1FFF;
        uint16_t val = raw & MCA_DATA_MASK;

        uint16_t diff = raw ^ prev;
        if (diff) {
            for (int b = 0; b < DIAG_NBITS; b++)
                if (diff & (1u << b)) toggles[b]++;
        }
        prev = raw;

        and_acc &= raw;
        or_acc  |= raw;
        if (val < vmin) vmin = val;
        if (val > vmax) vmax = val;
        sum += val;
        if (raw & MCA_OTR_MASK) otr++;
    }

    LOCK();
    memcpy(s_d.bit_toggles, toggles, sizeof(toggles));
    s_d.bit_always_1     = and_acc;
    s_d.bit_always_0     = (uint16_t)(~or_acc) & 0x1FFF;
    s_d.vmin             = vmin;
    s_d.vmax             = vmax;
    s_d.vmean            = (uint32_t)(sum / (n ? n : 1));
    s_d.otr_count        = otr;
    s_d.analyzed_samples = n;
    s_d.stats_valid      = true;
    UNLOCK();
}

void mca_diag_tick_1s(uint32_t expected_rate)
{
    LOCK();
    uint64_t delta = s_d.samples_total - s_prev_samples;
    s_prev_samples = s_d.samples_total;

    s_d.rate_measured = (uint32_t)delta;
    s_d.rate_expected = expected_rate;
    s_d.data_flowing  = (delta > 0);

    if (expected_rate > 0) {
        int64_t err = ((int64_t)delta - (int64_t)expected_rate) * 100
                      / (int64_t)expected_rate;
        s_d.rate_error_pct = (int32_t)err;
    } else {
        s_d.rate_error_pct = 0;
    }
    UNLOCK();
}

void mca_diag_get(mca_diag_t *out)
{
    LOCK();
    *out = s_d;
    UNLOCK();
}

size_t mca_diag_get_scope(uint16_t *dst, size_t max, size_t pre,
                          int32_t *trig_pos, int32_t *rise,
                          int32_t *age_ms, size_t *full_len)
{
    /* Снимок копируется БЕЗ блокировки: её задача обработки берёт на
     * каждом чанке, а копия - до 32 КБ. На время копии поднимаем
     * признак: публикация в этот момент буферы не меняет (см.
     * cap_publish), поэтому данные под нами не перезапишутся и ни
     * повторов, ни пустых ответов не нужно. */
    LOCK();
    const uint16_t *src = s_scope;
    const size_t    len = src ? s_scope_len : 0;
    const int32_t   tg  = s_scope_trig;
    const int32_t   rs  = s_scope_rise;
    const int64_t   tm  = s_scope_time;
    s_scope_rd = true;
    UNLOCK();

    size_t n  = len < max ? len : max;
    size_t st = 0;
    if (tg >= 0 && (size_t)tg > pre) st = (size_t)tg - pre;
    if (st + n > len) st = len - n;
    if (n) memcpy(dst, src + st, n * sizeof(uint16_t));

    LOCK();
    s_scope_rd = false;
    UNLOCK();

    *trig_pos = tg >= 0 ? tg - (int32_t)st : -1;
    *full_len = len;
    *rise     = rs;
    *age_ms   = tm ? (int32_t)((esp_timer_get_time() - tm) / 1000) : -1;
    return n;
}

void mca_diag_set_trig_level(int32_t lvl)
{
    if (lvl < 1) lvl = 1;
    s_trig_level = lvl;
}

int32_t mca_diag_get_trig_level(void) { return s_trig_level; }
