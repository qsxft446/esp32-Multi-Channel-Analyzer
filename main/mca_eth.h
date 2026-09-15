#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Ethernet на WIZnet W5500 по SPI - необязательный путь к прибору.
 *
 * Вызывать после esp_netif_init() и esp_event_loop_create_default().
 * ESP_OK - модуль найден, драйвер запущен, адрес придёт по DHCP. Любая
 * ошибка - модуля нет или он не отвечает: всё, что успели создать,
 * убрано, прибор работает без Ethernet. */
esp_err_t mca_eth_start(void);

bool      mca_eth_present(void);   /* модуль найден и запущен      */
bool      mca_eth_link_up(void);   /* кабель подключён             */
uint32_t  mca_eth_ip(void);        /* IPv4 (как в esp_ip4_addr_t), 0 - адреса нет */
