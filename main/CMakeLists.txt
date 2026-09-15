idf_component_register(
    SRCS "main.c"
         "adc_clk.c"
         "adc_cap.c"
         "mca_dsp.c"
         "mca_diag.c"
         "mca_prof.c"
         "mca_settings.c"
         "mca_web.c"
         "mca_eth.c"
         "mca_emu.c"
    INCLUDE_DIRS "."
    EMBED_FILES "logo.png"
    REQUIRES driver esp_driver_gpio esp_driver_ledc esp_driver_spi
             esp_driver_uart
             esp_hw_support esp_http_server esp_wifi esp_eth nvs_flash
             esp_netif esp_event esp_timer soc hal esp_mm
)
