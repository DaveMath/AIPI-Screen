#include "aipi_controls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

struct DebouncedButton {
    gpio_num_t pin;
    TickType_t debounce_ticks;
    bool raw_pressed;
    bool stable_pressed;
    TickType_t raw_changed_at;
};

DebouncedButton left_button = {
    .pin = kAipiLeftButtonPin,
    .debounce_ticks = pdMS_TO_TICKS(100),
    .raw_pressed = false,
    .stable_pressed = false,
    .raw_changed_at = 0,
};

DebouncedButton right_button = {
    .pin = kAipiRightButtonPin,
    .debounce_ticks = pdMS_TO_TICKS(200),
    .raw_pressed = false,
    .stable_pressed = false,
    .raw_changed_at = 0,
};

bool read_pressed(gpio_num_t pin) {
    return gpio_get_level(pin) == 0;
}

bool poll_button(DebouncedButton* button, TickType_t now) {
    const bool raw_pressed = read_pressed(button->pin);
    if (raw_pressed != button->raw_pressed) {
        button->raw_pressed = raw_pressed;
        button->raw_changed_at = now;
    }

    if (button->stable_pressed != button->raw_pressed &&
        now - button->raw_changed_at >= button->debounce_ticks) {
        button->stable_pressed = button->raw_pressed;
        return true;
    }
    return false;
}

}  // namespace

esp_err_t aipi_controls_init() {
    gpio_config_t inputs = {};
    inputs.pin_bit_mask = (1ULL << kAipiLeftButtonPin) | (1ULL << kAipiRightButtonPin);
    inputs.mode = GPIO_MODE_INPUT;
    inputs.pull_up_en = GPIO_PULLUP_ENABLE;
    inputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
    inputs.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t result = gpio_config(&inputs);
    if (result != ESP_OK) {
        return result;
    }

    const TickType_t now = xTaskGetTickCount();
    left_button.raw_pressed = read_pressed(left_button.pin);
    left_button.stable_pressed = left_button.raw_pressed;
    left_button.raw_changed_at = now;
    right_button.raw_pressed = read_pressed(right_button.pin);
    right_button.stable_pressed = right_button.raw_pressed;
    right_button.raw_changed_at = now;
    return ESP_OK;
}

AipiControlsState aipi_controls_poll() {
    const TickType_t now = xTaskGetTickCount();
    const bool left_changed = poll_button(&left_button, now);
    const bool right_changed = poll_button(&right_button, now);
    return {
        .left_pressed = left_button.stable_pressed,
        .right_pressed = right_button.stable_pressed,
        .left_changed = left_changed,
        .right_changed = right_changed,
    };
}

int aipi_controls_raw_level(gpio_num_t pin) {
    return gpio_get_level(pin);
}
