#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ЧАСЫ ПРИБОРА. Своих часов реального времени нет, поэтому время берётся:
 *  - по SNTP, если у прибора есть выход в интернет (WiFi или Ethernet);
 *  - иначе от браузера: любая страница прибора при открытии сверяет часы
 *    и присылает своё время, если часы не заданы или ушли больше чем на
 *    2 с. Время от SNTP браузер не перебивает.
 * Хранится UTC; местное время показывает страница. */

typedef enum {
    MCA_TIME_NONE    = 0,   /* не задано */
    MCA_TIME_SNTP    = 1,
    MCA_TIME_BROWSER = 2,
} mca_time_src_t;

/* Запустить SNTP (после esp_netif_init). */
void           mca_time_init(void);
/* Unix-время, мс; 0 - часы не заданы. */
int64_t        mca_time_now_ms(void);
mca_time_src_t mca_time_source(void);
/* Время от браузера. false - не принято (SNTP или время неправдоподобное). */
bool           mca_time_set_ms(int64_t unix_ms);
