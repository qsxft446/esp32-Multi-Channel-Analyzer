#include "mca_time.h"

#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "mca_time";

static volatile mca_time_src_t s_src = MCA_TIME_NONE;

static void sntp_cb(struct timeval *tv)
{
    if (s_src != MCA_TIME_SNTP)
        ESP_LOGI(TAG, "время получено по SNTP");
    s_src = MCA_TIME_SNTP;
}

void mca_time_init(void)
{
    /* Сервер из пула; если выхода в интернет нет, SNTP просто молча
     * повторяет попытки, и время приходит от браузера. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_cb;
    const esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "SNTP не запущен: %s - время только от браузера",
                 esp_err_to_name(e));
}

int64_t mca_time_now_ms(void)
{
    if (s_src == MCA_TIME_NONE) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

mca_time_src_t mca_time_source(void) { return s_src; }

bool mca_time_set_ms(int64_t unix_ms)
{
    if (s_src == MCA_TIME_SNTP) return false;
    if (unix_ms < 1600000000000LL) return false;       /* раньше 2020 года */
    if (s_src == MCA_TIME_BROWSER &&
        llabs(mca_time_now_ms() - unix_ms) < 2000) return false;
    const struct timeval tv = {
        .tv_sec  = (time_t)(unix_ms / 1000),
        .tv_usec = (suseconds_t)(unix_ms % 1000) * 1000,
    };
    if (settimeofday(&tv, NULL) != 0) return false;
    if (s_src != MCA_TIME_BROWSER)
        ESP_LOGI(TAG, "время получено от браузера");
    s_src = MCA_TIME_BROWSER;
    return true;
}
