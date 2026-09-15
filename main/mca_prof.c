#include "mca_prof.h"

#include "mca_config.h"
#include "mca_dsp.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "prof";

/* Спин-блокировка: под ней только копирование десятка слов. Держать
 * здесь мьютекс нельзя - mca_prof_chunk зовётся на каждом чанке из
 * задачи обработки, а её нельзя задерживать. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)

/* Накопление за текущую секунду. Пишет задача обработки (chunk/loss) и
 * задача веб-сервера (web_*) - разные поля, поэтому под блокировкой
 * только максимумы, а они обновляются за единицы тактов. */
static uint32_t s_chunks, s_q_max, s_wait_max, s_work_max, s_lost_1s;
/* Сумма времени обработки чанков за секунду: даёт честную загрузку
 * задачи обработки в ЛЮБОМ режиме. Показатель load из mca_dsp считается
 * только внутри обработки спектра и в режиме осциллографа равен нулю. */
static uint64_t s_work_sum_us;
static int64_t  s_busy_t0;
static uint32_t s_web_max[PROF_EP_CNT], s_web_cnt[PROF_EP_CNT];

/* Что обслуживает веб прямо сейчас - нужно журналу потерь. */
static volatile int      s_web_ep;
static volatile int64_t  s_web_t0;

/* Снимок за прошлую секунду: его отдаёт страница. */
static mca_prof_t s_snap;

/* Журнал потерь, кольцом. */
#define PROF_EV_MAX 12
static mca_prof_ev_t s_ev[PROF_EV_MAX];
static uint32_t      s_ev_head, s_ev_cnt;
/* всего событий и сколько из них уже напечатано в консоль */
static uint32_t      s_ev_total, s_ev_printed;

/* Названия запросов для консоли и страницы. */
static const char *const EP_NAME[PROF_EP_CNT] = {
    "нет", "/spectrum", "/scope", "/stat"
};

void mca_prof_chunk(uint32_t q_depth, uint32_t wait_us, uint32_t work_us)
{
    LOCK();
    s_chunks++;
    if (q_depth  > s_q_max)    s_q_max    = q_depth;
    if (wait_us  > s_wait_max) s_wait_max = wait_us;
    if (work_us  > s_work_max) s_work_max = work_us;
    s_work_sum_us += work_us;
    UNLOCK();
}

void mca_prof_loss(uint32_t lost_delta, uint32_t q_depth)
{
    if (!lost_delta) return;

    /* Контекст берём до блокировки: это просто чтение двух слов. */
    const int     ep = s_web_ep;
    const int64_t t0 = s_web_t0;
    const int64_t now = esp_timer_get_time();

    LOCK();
    s_lost_1s += lost_delta;
    mca_prof_ev_t *e = &s_ev[s_ev_head];
    e->t_ms    = (uint32_t)(now / 1000);
    e->lost    = lost_delta;
    e->q_depth = q_depth;
    e->web_ep  = (uint8_t)(ep >= 0 && ep < PROF_EP_CNT ? ep : PROF_EP_NONE);
    e->web_us  = (ep != PROF_EP_NONE && t0 > 0)
               ? (uint32_t)(now - t0) : 0;
    e->work_us = s_work_max;
    s_ev_head  = (s_ev_head + 1) % PROF_EV_MAX;
    if (s_ev_cnt < PROF_EV_MAX) s_ev_cnt++;
    s_ev_total++;
    UNLOCK();
}

void mca_prof_web_begin(int ep)
{
    s_web_t0 = esp_timer_get_time();
    s_web_ep = ep;
}

void mca_prof_web_end(int ep)
{
    const int64_t t0 = s_web_t0;
    s_web_ep = PROF_EP_NONE;
    if (ep <= PROF_EP_NONE || ep >= PROF_EP_CNT || t0 <= 0) return;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    LOCK();
    s_web_cnt[ep]++;
    if (us > s_web_max[ep]) s_web_max[ep] = us;
    UNLOCK();
}

/* ЗАГРУЗКА ЯДЕР ПО ЗАМЕРУ FreeRTOS.
 *
 * Своя строка в консоли:
 *   cpu0 X% cpu1 Y% | задача(ядро) Z% ...
 * Ядро: 0 или 1 - задача к нему привязана, * - может идти на любом.
 * Загрузка ядра = 100% минус доля его задачи простоя. Время
 * прерываний FreeRTOS засчитывает той задаче, которую они прервали,
 * так что обработчик захвата (он на ядре 0) виден как рост cpu0. */
#if MCA_PROF_LOG
#define PROF_TASKS_MAX 32
#define PROF_TOP       5

static void cpu_report(void)
{
    static TaskStatus_t ts[PROF_TASKS_MAX];
    static struct { UBaseType_t num; uint64_t rt; } prev[PROF_TASKS_MAX];
    static size_t  prev_n;
    static int64_t t_prev;

    configRUN_TIME_COUNTER_TYPE total;
    const UBaseType_t n = uxTaskGetSystemState(ts, PROF_TASKS_MAX, &total);
    const int64_t now = esp_timer_get_time();
    if (!n) {
        ESP_LOGW(TAG, "задач больше %d - загрузку ядер не считаю",
                 PROF_TASKS_MAX);
        return;
    }

    /* прирост времени каждой задачи за секунду */
    uint64_t d[PROF_TASKS_MAX];
    for (UBaseType_t i = 0; i < n; i++) {
        d[i] = 0;
        for (size_t k = 0; k < prev_n; k++) {
            if (prev[k].num == ts[i].xTaskNumber) {
                d[i] = ts[i].ulRunTimeCounter - prev[k].rt;
                break;
            }
        }
    }
    for (UBaseType_t i = 0; i < n; i++) {
        prev[i].num = ts[i].xTaskNumber;
        prev[i].rt  = ts[i].ulRunTimeCounter;
    }
    prev_n = n;

    const int64_t span = t_prev ? now - t_prev : 0;
    t_prev = now;
    if (span <= 0) return;

    uint64_t idle[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n; i++) {
        if (strncmp(ts[i].pcTaskName, "IDLE", 4) == 0 &&
            ts[i].xCoreID >= 0 && ts[i].xCoreID < 2)
            idle[ts[i].xCoreID] += d[i];
    }
    int32_t cpu[2];
    for (int c = 0; c < 2; c++) {
        int64_t v = 100 - (int64_t)(idle[c] * 100 / (uint64_t)span);
        cpu[c] = (int32_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    }

    /* самые занятые задачи, кроме простоя */
    char line[200];
    int  pos = 0;
    bool used[PROF_TASKS_MAX] = { false };
    for (int t = 0; t < PROF_TOP; t++) {
        int best = -1;
        for (UBaseType_t i = 0; i < n; i++) {
            if (used[i] || strncmp(ts[i].pcTaskName, "IDLE", 4) == 0)
                continue;
            if (best < 0 || d[i] > d[best]) best = (int)i;
        }
        if (best < 0) break;
        used[best] = true;
        const uint64_t pm = d[best] * 1000 / (uint64_t)span;
        if (pm < 5) break;               /* меньше 0.5% - не интересно */
        const BaseType_t core = ts[best].xCoreID;
        pos += snprintf(line + pos, sizeof(line) - pos, " %s(%c) %lu.%lu%%",
                        ts[best].pcTaskName,
                        core == 0 ? '0' : core == 1 ? '1' : '*',
                        (unsigned long)(pm / 10), (unsigned long)(pm % 10));
        if (pos >= (int)sizeof(line) - 1) break;
    }
    if (!pos) line[0] = 0;
    ESP_LOGI(TAG, "cpu0 %ld%% cpu1 %ld%% |%s", (long)cpu[0], (long)cpu[1],
             line);
}
#endif  /* MCA_PROF_LOG */

void mca_prof_tick_1s(void)
{
    /* Память и уровень сигнала читаем здесь: это не горячий путь. */
    const uint32_t hn = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t hm =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    wifi_ap_record_t ap;
    const int32_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                       ? ap.rssi : 0;
    const int64_t now = esp_timer_get_time();

    LOCK();
    const int64_t span = s_busy_t0 ? now - s_busy_t0 : 0;
    s_snap.busy_pm = span > 0 ? (uint32_t)(s_work_sum_us * 1000 / span) : 0;
    s_work_sum_us = 0;
    s_busy_t0     = now;
    /* «за секунду» - пересчётом на фактическое время тика: он опаздывает,
     * когда ядро 0 занято, и без пересчёта цифры завышались */
    s_snap.chunks = span > 0
        ? (uint32_t)(((uint64_t)s_chunks * 1000000ULL + (uint64_t)span / 2) /
                     (uint64_t)span)
        : s_chunks;
    s_snap.q_max       = s_q_max;
    s_snap.wait_max_us = s_wait_max;
    s_snap.work_max_us = s_work_max;
    s_snap.lost_1s = span > 0
        ? (uint32_t)(((uint64_t)s_lost_1s * 1000000ULL + (uint64_t)span / 2) /
                     (uint64_t)span)
        : s_lost_1s;
    for (int i = 0; i < PROF_EP_CNT; i++) {
        s_snap.web_max_us[i] = s_web_max[i];
        s_snap.web_cnt[i]    = s_web_cnt[i];
        s_web_max[i] = 0;
        s_web_cnt[i] = 0;
    }
    s_snap.heap_now = hn;
    s_snap.heap_min = hm;
    s_snap.rssi     = rssi;

    s_chunks = s_q_max = s_wait_max = s_work_max = s_lost_1s = 0;

    /* какие события потерь ещё не печатали (свежие - первыми) */
    uint32_t np = s_ev_total - s_ev_printed;
    if (np > PROF_EV_MAX) np = PROF_EV_MAX;
    mca_prof_ev_t nev[PROF_EV_MAX];
    for (uint32_t i = 0; i < np; i++)
        nev[i] = s_ev[(s_ev_head + PROF_EV_MAX - 1 - i) % PROF_EV_MAX];
    s_ev_printed = s_ev_total;
    const mca_prof_t sn = s_snap;
    UNLOCK();

    /* ---- ВЫВОД В КОНСОЛЬ ----
     * Одна строка в секунду плюс строка на каждую потерю. Сокращения:
     *   q     - пик очереди чанков из CAP_N_CHUNKS (полная = потери);
     *   work  - самая долгая обработка одного чанка, мс;
     *   wait  - самое долгое ожидание чанка задачей обработки, мс;
     *   sp/sc/st - запросов /spectrum, /scope, /stat за секунду x самый
     *              долгий из них, мс;
     *   heap  - свободная внутренняя память / её минимум за всё время;
     *   busy  - доля времени задачи обработки в работе, в любом режиме
     *           (load - то же, но только внутри обработки спектра). */
#if MCA_PROF_LOG
    mca_stats_t st;
    mca_dsp_get_stats(&st);
    ESP_LOGI(TAG,
             "q=%lu/%d work=%.2f wait=%.2f busy %lu%% lost=%lu | sp %lux%.0f "
             "sc %lux%.0f st %lux%.0f | heap %luk/%luk rssi %ld load %lu%% "
             "cps %lu",
             (unsigned long)sn.q_max, CAP_N_CHUNKS,
             sn.work_max_us / 1000.0, sn.wait_max_us / 1000.0,
             (unsigned long)(sn.busy_pm / 10),
             (unsigned long)sn.lost_1s,
             (unsigned long)sn.web_cnt[PROF_EP_SPEC],
             sn.web_max_us[PROF_EP_SPEC] / 1000.0,
             (unsigned long)sn.web_cnt[PROF_EP_SCOPE],
             sn.web_max_us[PROF_EP_SCOPE] / 1000.0,
             (unsigned long)sn.web_cnt[PROF_EP_STAT],
             sn.web_max_us[PROF_EP_STAT] / 1000.0,
             (unsigned long)(sn.heap_now / 1024),
             (unsigned long)(sn.heap_min / 1024),
             (long)sn.rssi, (unsigned long)(st.load_pm / 10),
             (unsigned long)st.cps);

    for (uint32_t i = np; i > 0; i--) {          /* печатаем от старых */
        const mca_prof_ev_t *e = &nev[i - 1];
        ESP_LOGW(TAG, "ПОТЕРЯ +%lu q=%lu/%d шёл %s %.0fms work=%.2f",
                 (unsigned long)e->lost, (unsigned long)e->q_depth,
                 CAP_N_CHUNKS,
                 EP_NAME[e->web_ep < PROF_EP_CNT ? e->web_ep : 0],
                 e->web_us / 1000.0, e->work_us / 1000.0);
    }

    cpu_report();
#else
    /* Без профиля - одна строка, и только если были потери: сколько и
     * какой запрос обслуживался при последней из них. */
    (void)sn;
    if (np) {
        uint32_t lost = 0;
        for (uint32_t i = 0; i < np; i++) lost += nev[i].lost;
        ESP_LOGW(TAG, "потеряно чанков: %lu, при последней потере шёл %s",
                 (unsigned long)lost,
                 EP_NAME[nev[0].web_ep < PROF_EP_CNT ? nev[0].web_ep : 0]);
    }
#endif
}

void mca_prof_get(mca_prof_t *out)
{
    LOCK();
    *out = s_snap;
    UNLOCK();
}

size_t mca_prof_events(mca_prof_ev_t *out, size_t max)
{
    LOCK();
    size_t n = s_ev_cnt < max ? s_ev_cnt : max;
    for (size_t i = 0; i < n; i++) {
        /* свежие первыми */
        uint32_t idx = (s_ev_head + PROF_EV_MAX - 1 - i) % PROF_EV_MAX;
        out[i] = s_ev[idx];
    }
    UNLOCK();
    return n;
}
