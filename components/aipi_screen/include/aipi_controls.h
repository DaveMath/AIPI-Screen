#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

constexpr gpio_num_t kAipiLeftButtonPin = GPIO_NUM_1;
constexpr gpio_num_t kAipiRightButtonPin = GPIO_NUM_42;

struct AipiControlsState {
    bool left_pressed;
    bool right_pressed;
    bool left_changed;
    bool right_changed;
};

esp_err_t aipi_controls_init();
AipiControlsState aipi_controls_poll();

// Prepare GPIO1 as the active-low EXT1 wake source before the application
// enters ESP32-S3 deep sleep. Call aipi_controls_init() after the next boot to
// return the pin to ordinary debounced button use.
esp_err_t aipi_controls_prepare_left_button_deep_sleep_wake();
int aipi_controls_raw_level(gpio_num_t pin);
