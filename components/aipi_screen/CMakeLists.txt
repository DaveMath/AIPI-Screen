idf_component_register(
    SRCS "aipi_audio.cpp" "aipi_battery.cpp" "aipi_controls.cpp" "status_led.cpp" "wifi_portal.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp_driver_i2s esp_driver_rmt esp_system esp_wifi esp_event esp_netif
             esp_http_server nvs_flash lwip json
)
