#pragma once
#include "esp_err.h"
#include "mca_config.h"
#include <stdbool.h>

esp_err_t mca_web_start(void);

/* Установлены из веба, читаются в main-задаче. */
extern volatile mca_mode_t mca_mode;
/* Что должно работать - раздельно для спектра и осциллографа. Захват
 * АЦП главная задача включает сама, когда он нужен открытой вкладке
 * (см. main.c): «Стоп» осциллографа не останавливает набор спектра и
 * наоборот. */
extern volatile bool       mca_spec_run;       /* набор спектра      */
extern volatile bool       mca_scope_run;      /* осциллограф идёт   */
extern volatile bool       mca_cmd_clear;
extern volatile int        mca_cmd_freq_idx;   /* -1 = нет запроса */
