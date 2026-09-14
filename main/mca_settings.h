#pragma once
#include "mca_config.h"

/* Настройки обработки и частота семплирования хранятся в NVS прибора:
 * переживают перезагрузку и одинаковы для любого браузера. */

/* Вызывать после инициализации NVS (её делает веб-модуль при старте WiFi)
 * и до начала захвата. */
void mca_settings_load(void);

void mca_settings_save_params(const mca_params_t *p);
void mca_settings_save_freq(int idx);
