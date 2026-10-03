#include "aipi_controls.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "aipi_controls";

struct DebouncedButton {
    gpio_num_t pin;
    TickType_t debounce_ticks;
    bool raw_pressed;
    bool stable_pressed;
    TickType_t raw_changed_at;
};

DebouncedButton left_button = {
    .pin = kAipiLeftButtonPin,
    .debounce_ticks = pdMS_TO_TICKS(35),
    .raw_pressed = false,
    .stable_pressed = false,
    .raw_changed_at = 0,
};

DebouncedButton right_button = {
    .pin = kAipiRightButtonPin,
    .debounce_ticks = pdMS_TO_TICKS(35),
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
        ESP_LOGI(kTag, "GPIO%d raw=%s", static_cast<int>(button->pin),
                 raw_pressed ? "pressed" : "released");
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
    // GPIO1 and GPIO42 can retain boot/debug configuration. Reset both pads
    // before assigning them to the AiPi's active-low buttons.
    rtc_gpio_deinit(kAipiLeftButtonPin);
    ESP_RETURN_ON_ERROR(gpio_reset_pin(kAipiLeftButtonPin), kTag,
                        "reset left button GPIO");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(kAipiRightButtonPin), kTag,
                        "reset right button GPIO");

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
    ESP_RETURN_ON_ERROR(gpio_pullup_en(kAipiLeftButtonPin), kTag,
                        "enable left button pull-up");
    ESP_RETURN_ON_ERROR(gpio_pullup_en(kAipiRightButtonPin), kTag,
                        "enable right button pull-up");

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

esp_err_t aipi_controls_prepare_left_button_deep_sleep_wake() {
    ESP_RETURN_ON_ERROR(rtc_gpio_init(kAipiLeftButtonPin), kTag,
                        "move left button to RTC domain");
    ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(kAipiLeftButtonPin,
                                               RTC_GPIO_MODE_INPUT_ONLY),
                        kTag, "set RTC wake pin input mode");
    ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(kAipiLeftButtonPin), kTag,
                        "enable RTC wake pin pull-up");
    ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(kAipiLeftButtonPin), kTag,
                        "disable RTC wake pin pull-down");
    ESP_RETURN_ON_ERROR(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,
                                             ESP_PD_OPTION_ON),
                        kTag, "keep RTC pull-up powered in deep sleep");

    const uint64_t wake_mask = 1ULL << kAipiLeftButtonPin;
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup(wake_mask,
                                                      ESP_EXT1_WAKEUP_ANY_LOW),
                        kTag, "enable left button EXT1 wake");
    ESP_LOGI(kTag, "GPIO%d prepared as active-low deep-sleep wake source",
             static_cast<int>(kAipiLeftButtonPin));
    return ESP_OK;
}

int aipi_controls_raw_level(gpio_num_t pin) {
    return gpio_get_level(pin);
}
