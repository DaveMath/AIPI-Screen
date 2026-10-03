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
int aipi_controls_raw_level(gpio_num_t pin);
