#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "mca_config.h"
#include "adc_clk.h"
#include "adc_cap.h"
#include "mca_dsp.h"
#include "mca_diag.h"
#include "mca_prof.h"
#include "esp_timer.h"
#include "mca_settings.h"
#include "mca_web.h"
#include "mca_emu.h"

static const char *TAG = "main";

static bool s_dsp_paused;

/* DSP-задача на ядре 1, отдельно от WiFi/HTTP (ядро 0). */
static void dsp_task(void *arg)
{
    const uint16_t *data;
    size_t n;

    /* Векторные команды проверяем здесь, в той же задаче и на том же
     * ядре, где потом идёт обработка. */
    mca_dsp_vec_init();

    uint64_t prev_lost = 0;

    while (1) {
        /* Замеры для профиля (страница «Диагностика»): сколько ждали
         * чанк, сколько его обрабатывали, сколько чанков стоит в
         * очереди и не выросли ли потери. Стоит это пары чтений
         * счётчика времени. */
        const int64_t t_wait0 = esp_timer_get_time();
        if (!adc_cap_wait_chunk(&data, &n, 100)) continue;
        const int64_t t_work0 = esp_timer_get_time();
        const uint32_t q_depth = (uint32_t)adc_cap_queue_depth();

        /* РАБОТАЕМ ТОЛЬКО НАД ТЕМ, ЧТО ОТКРЫТО.
         *
         * В режиме осциллографа спектр не нужен, а полный фильтр
         * на каждом отсчёте - самая дорогая часть. Раньше он
         * крутился всегда, независимо от того, что на экране.
         * Трапецию для картинки осциллографа считает браузер
         * по этому же снимку. */
        mca_mode_t mode = mca_mode;
        bool need_dsp   = (mode == MCA_MODE_SPECTRUM ||
                           mode == MCA_MODE_PULSE);
        diag_scope_mode_t sm =
              (mode == MCA_MODE_SCOPE)      ? DIAG_SCOPE_AUTO
            : (mode == MCA_MODE_SCOPE_TRIG) ? DIAG_SCOPE_NORMAL
            :                                 DIAG_SCOPE_OFF;

        if (adc_cap_take_gap()) { mca_dsp_flush(); mca_diag_flush(); }
        mca_diag_feed(data, n, sm);

        if (need_dsp) {
            mca_dsp_process(data, n);
        } else if (!s_dsp_paused) {
            /* уходим из режима набора - при возврате фильтру
             * понадобится прогрев, поток для него разорвался */
            mca_dsp_flush();
        }
        s_dsp_paused = !need_dsp;

        const int64_t t_end = esp_timer_get_time();
        mca_prof_chunk(q_depth, (uint32_t)(t_work0 - t_wait0),
                       (uint32_t)(t_end - t_work0));
        const uint64_t lost = adc_cap_chunks_lost();
        if (lost != prev_lost) {
            mca_prof_loss((uint32_t)(lost - prev_lost), q_depth);
            prev_lost = lost;
        }

        /* Принудительной паузы здесь НЕТ.
         *
         * Она уступала ядро на системный тик каждые 64 чанка,
         * но при 8 МГц за эту миллисекунду контроллер успевал
         * заполнить почти все буферы, и кольцо затирало
         * неразобранное - отсюда были постоянные 2% потерь.
         *
         * Пауза и не нужна: когда обработка успевает, задача
         * сама блокируется на ожидании следующего чанка, и
         * ядро освобождается. А проверка задачи простоя на
         * этом ядре отключена в конфигурации, потому что ядро
         * намеренно отдано под обработку потока. */
    }
}

static void capture_start(void)
{
    esp_err_t e = adc_cap_start();
    if (e != ESP_OK)
        ESP_LOGE(TAG, "старт LCD_CAM с ошибкой: %s — смотрите /diag",
                 esp_err_to_name(e));
}

static void capture_stop(void)
{
    adc_cap_stop();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== MCA на AD9226 + ESP32-S3 ===");

    mca_dsp_init();
    mca_diag_init();

    ESP_ERROR_CHECK(adc_clk_start(mca_freq_table[MCA_FREQ_DEFAULT_IDX]));
    ESP_ERROR_CHECK(adc_cap_init());
    ESP_ERROR_CHECK(mca_web_start());
    mca_settings_load();        /* NVS к этому моменту поднят стартом WiFi */
    /* Эмуляция MCA на UART0, если включена: с этого места консоль молчит. */
    mca_emu_init();

    xTaskCreatePinnedToCore(dsp_task, "dsp", 8192, NULL, 10, NULL, 1);

    ESP_LOGI(TAG, "свободно внутр. RAM: %u, PSRAM: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Секундный тик - по часам, а не счётчиком «10 проходов по 100 мс»:
     * у главной задачи самый низкий приоритет, и под нагрузкой ядра 0
     * проходы растягиваются. Сами «в секунду» считаются по фактическому
     * времени (см. mca_diag_tick_1s, mca_dsp_tick_1s, mca_prof_tick_1s). */
    int64_t next_tick_us = esp_timer_get_time() + 1000000;

    while (1) {
        if (mca_cmd_clear) { mca_cmd_clear = false; mca_dsp_reset_spectrum(); }

        if (mca_cmd_freq_idx >= 0) {
            int idx = mca_cmd_freq_idx;
            mca_cmd_freq_idx = -1;
            if (idx < MCA_FREQ_COUNT) {
                bool was = adc_cap_is_running();
                capture_stop();
                adc_clk_set_freq(mca_freq_table[idx]);
                mca_settings_save_freq(idx);
                vTaskDelay(pdMS_TO_TICKS(20));
                if (was) capture_start();
            }
        }

        /* ЗАХВАТ ВКЛЮЧЁН ТОГДА, КОГДА ОН НУЖЕН ОТКРЫТОЙ ВКЛАДКЕ.
         *
         * Раньше «Старт/Стоп» запускали и останавливали сам захват, общий
         * для всех вкладок: остановили осциллограф - встал и набор
         * спектра, запустили осциллограф - спектр пошёл набираться после
         * возврата на свою вкладку. Теперь у спектра и осциллографа свои
         * признаки, а захват здесь подгоняется под открытую вкладку. В
         * режиме осциллографа спектр не набирается (см. dsp_task), так
         * что работающий для осциллографа захват спектр не трогает. */
        {
            const mca_mode_t m = mca_mode;
            const bool scope = (m == MCA_MODE_SCOPE ||
                                m == MCA_MODE_SCOPE_TRIG);
            const bool want  = scope ? mca_scope_run : mca_spec_run;
            if (want != adc_cap_is_running()) {
                if (want) capture_start();
                else      capture_stop();
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_tick_us) {
            next_tick_us += 1000000;
            if (now_us - next_tick_us > 1000000)   /* сильно отстали - не догоняем */
                next_tick_us = now_us + 1000000;
            mca_dsp_tick_1s();
            mca_diag_tick_1s(adc_clk_get_freq());
            mca_prof_tick_1s();

            /* Если захват включён, а чанки не идут - повторяем фронт
             * кадровой синхронизации. Возможно, один фронт запускает
             * ровно один кадр, и после него модуль снова ждёт старта. */
            if (adc_cap_is_running()) {
                static uint64_t prev_chunks;
                uint64_t now_chunks = adc_cap_chunks_total();
                if (now_chunks == prev_chunks) adc_cap_kick();
                prev_chunks = now_chunks;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
