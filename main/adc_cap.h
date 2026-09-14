#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Захват параллельной шины AD9226 через LCD_CAM + GDMA.
 *
 * Работаем в "камерном" режиме без реального сенсора:
 *  - PCLK берём с GPIO18 (перемычка с нашего же CLK GPIO5);
 *  - VSYNC / HREF / DE прибиваем к константам через GPIO-матрицу,
 *    т.к. кадров и строк у нас нет - поток непрерывный;
 *  - cam_rec_data_bytelen задаёт, через сколько байт CAM выставит
 *    EOF, т.е. как часто дёргается наш колбэк.
 *
 * Колбэк вызывается из ISR. Он только кладёт индекс готового чанка
 * в очередь - вся обработка в задаче.
 */

typedef void (*cap_chunk_cb_t)(const uint16_t *data, size_t n_samples,
                               void *ctx);

esp_err_t adc_cap_init(void);
esp_err_t adc_cap_start(void);
esp_err_t adc_cap_stop(void);
bool      adc_cap_is_running(void);

/* Забрать самый свежий готовый чанк (для режима осциллографа).
 * Копирует в dst до max_samples отсчётов, возвращает сколько скопировал. */
size_t    adc_cap_snapshot(uint16_t *dst, size_t max_samples);

/* Очередь готовых чанков для DSP-задачи. */
bool      adc_cap_wait_chunk(const uint16_t **out, size_t *n, uint32_t ms);
uint64_t  adc_cap_chunks_lost(void);
uint64_t  adc_cap_chunks_total(void);
/* Сколько чанков ждёт разбора (столько же, сколько буферов = потери). */
size_t    adc_cap_queue_depth(void);

/* Был ли пропуск чанков с прошлого вызова (поток разорван). */
bool      adc_cap_take_gap(void);

/* Сырые регистры CAM - чтобы видеть, что реально записалось,
 * а не что мы думаем, что записали. */
void      adc_cap_dump_regs(uint32_t *ctrl, uint32_t *ctrl1, int *pclk_lvl);

/* Повторить старт кадра, если поток встал. */
void      adc_cap_kick(void);

/* Какие линии данных шевелятся. Возвращает маску менявшихся бит. */
uint32_t  adc_cap_probe_data_lines(uint32_t *stuck_hi, uint32_t *stuck_lo);
