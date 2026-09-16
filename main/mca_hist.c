#include "mca_hist.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "mca_hist";

/* Отсчёт с номером s лежит в ячейке s % MCA_HIST_LEN. Пишет только
 * главная задача, читает веб. Мьютекс, а не спин-блокировка: копия до
 * 10 КБ из PSRAM, а задача обработки спектра его не берёт - её он не
 * задерживает. */
static mca_hist_sample_t *s_ring;
static uint32_t           s_next;    /* номер следующего отсчёта */
static uint32_t           s_count;   /* сколько лежит            */
static uint32_t           s_epoch;
static SemaphoreHandle_t  s_mx;

static uint32_t new_epoch(void)
{
    return esp_random() | 1u;        /* 0 у страницы значит «ещё не знаю» */
}

bool mca_hist_init(void)
{
    s_mx   = xSemaphoreCreateMutex();
    s_ring = heap_caps_calloc(MCA_HIST_LEN, sizeof(mca_hist_sample_t),
                              MALLOC_CAP_SPIRAM);
    s_epoch = new_epoch();
    if (!s_mx || !s_ring) {
        ESP_LOGE(TAG, "нет памяти под историю CPS");
        return false;
    }
    return true;
}

void mca_hist_push(uint32_t counts, uint32_t dur_ms)
{
    if (!s_ring) return;
    const mca_hist_sample_t smp = {
        .end_ds = (uint32_t)(esp_timer_get_time() / 100000),
        .counts = counts,
        .dur_ms = (uint16_t)(dur_ms > 65535 ? 65535 : dur_ms),
    };
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_ring[s_next % MCA_HIST_LEN] = smp;
    s_next++;
    if (s_count < MCA_HIST_LEN) s_count++;
    xSemaphoreGive(s_mx);
}

void mca_hist_clear(void)
{
    if (!s_ring) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_count = 0;
    s_epoch = new_epoch();
    xSemaphoreGive(s_mx);
}

size_t mca_hist_copy(uint32_t from, mca_hist_sample_t *out, size_t max,
                     uint32_t *first, uint32_t *next, uint32_t *epoch)
{
    if (!s_ring) {
        *first = *next = 0;
        *epoch = 0;
        return 0;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const uint32_t nx = s_next;
    const uint32_t oldest = nx - s_count;
    if (from < oldest || from > nx) from = oldest;
    size_t n = nx - from;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++)
        out[i] = s_ring[(from + i) % MCA_HIST_LEN];
    *first = from;
    *next  = nx;
    *epoch = s_epoch;
    xSemaphoreGive(s_mx);
    return n;
}
