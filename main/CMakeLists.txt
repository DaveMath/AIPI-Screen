idf_component_register(
    SRCS "main.cpp" "aipi_audio.cpp" "aipi_battery.cpp" "aipi_controls.cpp" "wifi_portal.cpp" "status_led.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_driver_i2s esp_driver_rmt esp_system spi_flash esp_wifi esp_event esp_netif
             esp_http_server nvs_flash lwip json
)
