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

/* Предыстория НЕ копится отдельным массивом: держим указатели на
 * DIAG_PRV_N предыдущих чанков прямо в буферах захвата. Раньше на каждый
 * чанк приходилось до 6 КБ копий (memmove + memcpy), а при 16 МГц весь
 * бюджет на чанк - 128 мкс. Буферы захвата живут 16 чанков (2 мс), а
 * предыстория копируется сразу при срабатывании, так что затереть её
 * не успевают (см. prv_safe). */
#define DIAG_PRV_N 4
_Static_assert(DIAG_SCOPE_PRE <= DIAG_PRV_N * CAP_CHUNK_SAMPLES,
               "предыстория длиннее запомненных чанков");
static const uint16_t *s_prv[DIAG_PRV_N];
static size_t   s_prv_n[DIAG_PRV_N];
static int      s_prv_i;          /* куда писать следующий */

/* Длина окна: сколько просит страница (s_want) плюс предыстория. Задаётся
 * при начале сборки, чтобы смена развёртки не рвала собираемое окно. */
static volatile size_t s_want = DIAG_SCOPE_LEN;
static size_t   s_cap_len = DIAG_SCOPE_LEN;

/* СИНХРОНИЗАЦИЯ ПО АМПЛИТУДЕ. После фронта прибор меряет высоту
 * импульса и, если она вне [s_amin, s_amax], отбрасывает кадр и ждёт
 * следующий фронт. s_amax <= 0 - фильтр выключен (высота всё равно
 * мерится и показывается). */
#define AMP_WIN   256             /* где после фронта искать вершину, отсч */
#define AMP_BASE  64              /* по скольким отсчётам до фронта база   */
_Static_assert(DIAG_SCOPE_PRE + AMP_WIN <= DIAG_SCOPE_LEN, "окно короче поиска");
static volatile int32_t s_amin, s_amax;
static bool     s_amp_done;       /* высота этого кадра уже измерена       */
static int32_t  s_cap_amp = -1;
static uint32_t s_rej;            /* отброшено с прошлой публикации        */
static int32_t  s_scope_amp = -1;
static uint32_t s_scope_rej;

static enum { SC_IDLE, SC_ARMED, SC_POST } s_sc;
static size_t   s_fill;
static int32_t  s_cap_trig, s_cap_rise;
static int64_t  s_arm_us;
static int64_t  s_snap_next_us;
static int32_t  s_trig_level = 30;

void mca_diag_init(void)
{
    memset(&s_d, 0, sizeof(s_d));

    /* Оба буфера окна по 64 КБ - в PSRAM: во внутреннюю память не
     * влезают. Пишется в них только нужная развёртке длина. */
    s_scope = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_SPIRAM);
    s_build = heap_caps_calloc(DIAG_SCOPE_LEN, 2, MALLOC_CAP_SPIRAM);
    if (!s_scope || !s_build)
        ESP_LOGE(TAG, "нет памяти под окно осциллографа");
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
    s_prv_i = (s_prv_i + 1) % DIAG_PRV_N;
}

/* k-й предыдущий чанк: 0 - самый свежий */
#define PRV(k) ((s_prv_i + DIAG_PRV_N - 1 - (k)) % DIAG_PRV_N)

static void prv_reset(void)
{
    for (int k = 0; k < DIAG_PRV_N; k++) s_prv_n[k] = 0;
}

/* Сколько отсчётов подряд, без разрыва, лежит в запомненных чанках. */
static size_t prv_avail(void)
{
    size_t s = 0;
    for (int k = 0; k < DIAG_PRV_N; k++) {
        const size_t pn = s_prv_n[PRV(k)];
        if (!pn) break;
        s += pn;
    }
    return s;
}

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
    size_t take = s_cap_len - s_fill;
    if (take > n) take = n;
    memcpy(s_build + s_fill, d, take * 2);
    s_fill += take;
    return s_fill >= s_cap_len;
}

/* Начать окно так, чтобы отсчёт d[j] встал после предыстории.
 * Предыстория собирается здесь, ОДИН раз на срабатывание: из
 * запомненных предыдущих чанков (от старых к свежим), затем из начала
 * текущего. */
static bool cap_start(const uint16_t *d, size_t n, size_t j)
{
    /* длина окна: запрос страницы + предыстория, в пределах буфера */
    size_t len = s_want + DIAG_SCOPE_PRE;
    if (len < DIAG_SCOPE_PRE + AMP_WIN) len = DIAG_SCOPE_PRE + AMP_WIN;
    if (len > DIAG_SCOPE_LEN)           len = DIAG_SCOPE_LEN;
    s_cap_len = len;

    size_t need = DIAG_SCOPE_PRE;
    const size_t cur = j < need ? j : need;
    need -= cur;

    size_t take[DIAG_PRV_N] = { 0 };
    int used = 0;
    for (int k = 0; k < DIAG_PRV_N && need; k++) {
        const size_t pn = s_prv_n[PRV(k)];
        if (!pn) break;                  /* дальше - разрыв потока */
        take[k] = pn < need ? pn : need;
        need   -= take[k];
        used    = k + 1;
    }

    size_t pos = 0;
    for (int k = used - 1; k >= 0; k--) {
        const int i = PRV(k);
        memcpy(s_build + pos, s_prv[i] + s_prv_n[i] - take[k], take[k] * 2);
        pos += take[k];
    }
    if (cur) {
        memcpy(s_build + pos, d + j - cur, cur * 2);
        pos += cur;
    }
    s_fill     = pos;
    s_cap_trig = (int32_t)pos;
    s_amp_done = false;
    s_cap_amp  = -1;
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
        s_scope_len  = s_fill;
        s_scope_trig = s_cap_trig;
        s_scope_rise = s_cap_rise;
        s_scope_time = now;
        s_scope_amp  = s_cap_amp;
        s_scope_rej  = s_rej;
        s_rej        = 0;
    }
    UNLOCK();
    s_sc = SC_IDLE;
    s_snap_next_us = now + SNAP_PERIOD_US;
}

/* Высота импульса в собираемом окне: максимум за AMP_WIN отсчётов после
 * фронта минус среднее AMP_BASE отсчётов до него (с отступом 8 - там уже
 * может начинаться фронт), с учётом полярности. -1 - после фронта ещё
 * мало отсчётов, решим на следующем чанке. */
static int32_t amp_measure(void)
{
    const size_t t   = (size_t)s_cap_trig;
    const size_t end = t + AMP_WIN;
    if (s_fill < end && s_fill < s_cap_len) return -1;
    const size_t e = end < s_fill ? end : s_fill;

    const size_t b1 = t > 8 ? t - 8 : 0;
    const size_t b0 = b1 > AMP_BASE ? b1 - AMP_BASE : 0;
    int32_t base;
    if (b1 > b0) {
        int32_t s = 0;
        for (size_t i = b0; i < b1; i++) s += s_build[i] & MCA_DATA_MASK;
        base = s / (int32_t)(b1 - b0);
    } else {
        base = s_build[t] & MCA_DATA_MASK;
    }

    const int32_t sg = mca_dsp_polarity_neg() ? -1 : 1;
    int32_t pk = 0;
    for (size_t i = t; i < e; i++) {
        const int32_t v = sg * ((int32_t)(s_build[i] & MCA_DATA_MASK) - base);
        if (v > pk) pk = v;
    }
    return pk;
}

/* Фильтр амплитуды. true - кадр собирается дальше; false - высота вне
 * диапазона, кадр отброшен и прибор снова ждёт фронт (s_arm_us прежний:
 * в авто свободный пуск наступит в срок, если подходящих импульсов нет).
 * Решение принимается, как только после фронта набралось AMP_WIN
 * отсчётов - не дожидаясь конца длинного окна. */
static bool amp_gate(void)
{
    if (s_amp_done || s_cap_trig < 0) return true;
    const int32_t a = amp_measure();
    if (a < 0) return true;
    s_amp_done = true;
    s_cap_amp  = a;
    const int32_t lo = s_amin, hi = s_amax;
    if (hi > 0 && (a < lo || a > hi)) {
        s_rej++;
        s_sc = SC_ARMED;
        return false;
    }
    return true;
}

/* Хватит ли предыстории из буферов захвата, пока их не перезаписал DMA.
 * Кольцо буферов захвата крутится само, без разрешения задачи
 * обработки: буфер самого старого из DIAG_PRV_N запомненных чанков снова
 * пишется, когда в очереди скопилось CAP_N_CHUNKS - DIAG_PRV_N чанков. С
 * запасом в два: при очереди глубже срабатывание пропускаем (дождёмся
 * следующего фронта), а в свободном пуске берём окно без предыстории. */
static bool prv_safe(void)
{
    return adc_cap_queue_depth() <= CAP_N_CHUNKS - (DIAG_PRV_N + 2);
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
        const bool full = cap_take(d, n);
        if (!amp_gate()) return;        /* вне диапазона - снова ждём */
        if (full) cap_publish(now);
        return;
    }

    /* Фронт ищем, только когда предыстория набрана ЦЕЛИКОМ. Иначе момент
     * синхронизации встал бы левее обычного, и первые кадры после запуска
     * захвата (а он перезапускается при каждой смене вкладки и после
     * паузы) прыгали бы по экрану. Набирается за DIAG_SCOPE_PRE отсчётов -
     * при 16 МГц это 0.4 мс. */
    if (prv_avail() < DIAG_SCOPE_PRE) return;

    /* SC_ARMED: ищем пересечение уровня */
    const int32_t lvl = s_trig_level;
    const size_t  K   = DIAG_TRIG_SPAN;
    /* импульсы вниз - синхронизация по фронту вниз */
    const int32_t sg  = mca_dsp_polarity_neg() ? -1 : 1;
    const uint16_t *pv = s_prv[PRV(0)];
    const size_t    pvn = s_prv_n[PRV(0)];

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
        const bool full = cap_start(d, n, j);
        if (!amp_gate()) return;        /* вне диапазона - ждём следующий */
        if (full) cap_publish(now);
        else      s_sc = SC_POST;
        return;
    }

    if (sm == DIAG_SCOPE_AUTO && now - s_arm_us > AUTO_WAIT_US) {
        /* фронта нет - свободный пуск, чтобы экран не замер */
        s_cap_rise = 0;
        if (!prv_safe()) prv_reset();
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
 * осциллографа старые указатели ведут в уже перезаписанные буферы -
 * поэтому prv_reset(). */
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

void mca_diag_set_scope_want(size_t n)
{
    if (n > DIAG_SCOPE_LEN) n = DIAG_SCOPE_LEN;
    s_want = n;
}

void mca_diag_set_amp_window(int32_t lo, int32_t hi)
{
    if (lo < 0) lo = 0;
    if (hi > 0 && lo > hi) { int32_t t = lo; lo = hi; hi = t; }
    s_amin = lo;
    s_amax = hi;
}

size_t mca_diag_get_scope(uint16_t *dst, size_t max, size_t pre,
                          int32_t *trig_pos, int32_t *rise,
                          int32_t *age_ms, size_t *full_len,
                          int32_t *amp, uint32_t *rej)
{
    /* Снимок копируется БЕЗ блокировки: её задача обработки берёт на
     * каждом чанке, а копия - до 64 КБ. На время копии поднимаем
     * признак: публикация в этот момент буферы не меняет (см.
     * cap_publish), поэтому данные под нами не перезапишутся и ни
     * повторов, ни пустых ответов не нужно. */
    LOCK();
    const uint16_t *src = s_scope;
    const size_t    len = src ? s_scope_len : 0;
    const int32_t   tg  = s_scope_trig;
    const int32_t   rs  = s_scope_rise;
    const int64_t   tm  = s_scope_time;
    *amp = s_scope_amp;
    *rej = s_scope_rej;
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
