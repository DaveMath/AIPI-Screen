idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES aipi_screen driver esp_system spi_flash esp_wifi nvs_flash
)
