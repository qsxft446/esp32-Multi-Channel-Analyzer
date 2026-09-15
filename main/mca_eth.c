#include "mca_eth.h"
#include "mca_config.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "mca_eth";

static esp_eth_handle_t  s_eth;
static volatile bool     s_link;
static volatile uint32_t s_ip;       /* 0 - адреса нет */

static void eth_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        s_link = true;
        ESP_LOGI(TAG, "Ethernet: кабель подключён, жду адрес от роутера");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        s_link = false;
        s_ip   = 0;
        ESP_LOGW(TAG, "Ethernet: кабель отключён");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip.addr;
        ESP_LOGI(TAG, "Ethernet подключён: http://" IPSTR "/",
                 IP2STR(&e->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        s_ip = 0;
        ESP_LOGW(TAG, "Ethernet: адрес потерян");
    }
}

esp_err_t mca_eth_start(void)
{
#if !MCA_ETH_ENABLE
    ESP_LOGI(TAG, "Ethernet выключен в mca_config.h (MCA_ETH_ENABLE)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err;

    /* Прерывание W5500 обслуживает общий сервис прерываний GPIO. Если он
     * уже установлен кем-то ещё - это не ошибка. */
    bool isr_by_us = false;
    if (PIN_ETH_INT >= 0) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK) {
            isr_by_us = true;
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "сервис прерываний GPIO: %s", esp_err_to_name(err));
            return err;
        }
    }

    const spi_bus_config_t bus = {
        .miso_io_num   = PIN_ETH_MISO,
        .mosi_io_num   = PIN_ETH_MOSI,
        .sclk_io_num   = PIN_ETH_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "шина SPI для W5500: %s", esp_err_to_name(err));
        if (isr_by_us) gpio_uninstall_isr_service();
        return err;
    }

    spi_device_interface_config_t dev = {
        .mode           = 0,
        .clock_speed_hz = MCA_ETH_SPI_MHZ * 1000 * 1000,
        .queue_size     = 20,
        .spics_io_num   = PIN_ETH_CS,
    };
    eth_w5500_config_t wcfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
    wcfg.int_gpio_num   = PIN_ETH_INT;
    wcfg.poll_period_ms = PIN_ETH_INT >= 0 ? 0 : 10;

    /* Задачу приёма - на ядро, где создаётся драйвер: сеть поднимается из
     * app_main, это ядро 0. Иначе задача с приоритетом 15 могла бы
     * вытеснять обработку спектра (приоритет 10) на ядре 1. */
    eth_mac_config_t mcfg = ETH_MAC_DEFAULT_CONFIG();
    mcfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    eth_phy_config_t pcfg = ETH_PHY_DEFAULT_CONFIG();
    pcfg.phy_addr       = 1;
    pcfg.reset_gpio_num = PIN_ETH_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&wcfg, &mcfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&pcfg);
    esp_eth_handle_t h = NULL;
    if (mac && phy) {
        esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac, phy);
        err = esp_eth_driver_install(&ecfg, &h);
    } else {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        /* Модуля нет или он не отвечает: драйвер после сброса читает
         * версию чипа и не находит её. Всё созданное убираем. */
        ESP_LOGW(TAG, "W5500 не найден или не отвечает (%s) - работаем "
                      "без Ethernet", esp_err_to_name(err));
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        spi_bus_free(SPI2_HOST);
        if (isr_by_us) gpio_uninstall_isr_service();
        return err;
    }

    /* У W5500 нет своего MAC-адреса - берём адрес Ethernet из ESP32. */
    uint8_t addr[6];
    if (esp_read_mac(addr, ESP_MAC_ETH) == ESP_OK)
        esp_eth_ioctl(h, ETH_CMD_S_MAC_ADDR, addr);

    const esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    esp_netif_attach(netif, esp_eth_new_netif_glue(h));
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_ev, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_ev, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, eth_ev, NULL);

    err = esp_eth_start(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "запуск Ethernet: %s", esp_err_to_name(err));
        return err;
    }
    s_eth = h;
    ESP_LOGI(TAG, "W5500 найден (SPI %d МГц), адрес придёт по DHCP",
             MCA_ETH_SPI_MHZ);
    return ESP_OK;
#endif
}

bool     mca_eth_present(void) { return s_eth != NULL; }
bool     mca_eth_link_up(void) { return s_link; }
uint32_t mca_eth_ip(void)      { return s_ip; }
