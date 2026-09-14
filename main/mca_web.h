#pragma once
#include "esp_err.h"
#include "mca_config.h"
#include <stdbool.h>

esp_err_t mca_web_start(void);

/* Установлены из веба, читаются в main-задаче. */
extern volatile mca_mode_t mca_mode;
extern volatile bool       mca_cmd_start;
extern volatile bool       mca_cmd_stop;
extern volatile bool       mca_cmd_clear;
extern volatile int        mca_cmd_freq_idx;   /* -1 = нет запроса */
