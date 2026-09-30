#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t status_led_init();
esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness);

