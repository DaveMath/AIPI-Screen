#include <array>
#include <stdint.h>

#include "aipi_audio.h"
#include "aipi_battery.h"
#include "aipi_controls.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

namespace {

constexpr char kTag[] = "aipi_screen";

// Optional reusable power-control pattern. Keep disabled in the standalone
// hardware diagnostic; applications can enable it in their build settings.
#ifndef AIPI_ENABLE_LEFT_SHUTDOWN
#define AIPI_ENABLE_LEFT_SHUTDOWN 0
#endif

constexpr gpio_num_t kBacklight = GPIO_NUM_3;
constexpr gpio_num_t kI2cScl = GPIO_NUM_4;
constexpr gpio_num_t kI2cSda = GPIO_NUM_5;
constexpr gpio_num_t kLcdDc = GPIO_NUM_7;
constexpr gpio_num_t kLcdCs = GPIO_NUM_15;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_16;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_17;
constexpr gpio_num_t kLcdReset = GPIO_NUM_18;
constexpr uint8_t kEs8311Address = 0x18;

constexpr int kDisplayWidth = 128;
constexpr int kDisplayHeight = 128;
constexpr uint8_t kMadctl = 0x68;

spi_device_handle_t lcd = nullptr;
AipiBatteryReading battery_reading = {};
bool battery_ready = false;
bool battery_bar_visible = true;
constexpr uint8_t kAudioLevels[] = {0, 10, 50, 100};
constexpr uint8_t kScreenTimeoutMinutes[] = {1, 5, 0};
constexpr TickType_t kScreenTimeoutHoldTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kScreenTimeoutCycleTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kScreenSaveConfirmationTicks = pdMS_TO_TICKS(1500);
constexpr TickType_t kShutdownHoldTicks = pdMS_TO_TICKS(3000);
constexpr TickType_t kShutdownCountdownTicks = pdMS_TO_TICKS(3000);
size_t audio_level_index = 0;
size_t screen_timeout_index = 0;
TickType_t last_battery_read = 0;
TickType_t last_battery_blink = 0;
TickType_t screen_last_activity = 0;
TickType_t right_pressed_at = 0;
TickType_t right_next_cycle_at = 0;
TickType_t left_pressed_at = 0;
TickType_t left_shutdown_countdown_started_at = 0;
uint8_t left_shutdown_countdown_shown = 0xff;
TickType_t screen_confirmation_until = 0;
bool right_long_press_active = false;
bool left_shutdown_countdown_active = false;
bool left_shutdown_armed = false;
bool screen_backlight_on = true;
bool left_wake_only = false;
bool right_wake_only = false;
bool screen_overlay_active = false;

void lcd_write(bool data, const void* bytes, size_t length) {
    gpio_set_level(kLcdDc, data ? 1 : 0);
    spi_transaction_t transaction = {};
    transaction.length = length * 8;
    transaction.tx_buffer = bytes;
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &transaction));
}

void lcd_command(uint8_t command) {
    lcd_write(false, &command, 1);
}

void lcd_data(const void* bytes, size_t length) {
    lcd_write(true, bytes, length);
}

void lcd_command_data(uint8_t command, const uint8_t* bytes, size_t length) {
    lcd_command(command);
    if (length != 0) {
        lcd_data(bytes, length);
    }
}

void lcd_reset() {
    gpio_set_level(kLcdReset, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(kLcdReset, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void lcd_init() {
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << kBacklight) | (1ULL << kLcdDc) | (1ULL << kLcdReset);
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_set_level(kBacklight, 0);

    spi_bus_config_t bus = {};
    bus.sclk_io_num = kLcdSclk;
    bus.mosi_io_num = kLcdMosi;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = kDisplayWidth * 16 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t device = {};
    device.clock_speed_hz = 20 * 1000 * 1000;
    device.mode = 0;
    device.spics_io_num = kLcdCs;
    device.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &lcd));

    lcd_reset();
    lcd_command(0x01);  // Software reset
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_command(0x11);  // Sleep out
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t color_mode[] = {0x05};  // 16-bit RGB565
    lcd_command_data(0x3A, color_mode, sizeof(color_mode));
    const uint8_t orientation[] = {kMadctl};
    lcd_command_data(0x36, orientation, sizeof(orientation));
    lcd_command(0x20);  // Display inversion off
    lcd_command(0x29);  // Display on
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(kBacklight, 1);
}

void lcd_fill_rect(int x, int y, int width, int height, uint16_t rgb565) {
    // This 128x128 panel uses controller coordinates directly: no X/Y offset.
    const int column_end = x + width - 1;
    const int row_end = y + height - 1;
    const uint8_t columns[] = {
        static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
        static_cast<uint8_t>(column_end >> 8), static_cast<uint8_t>(column_end),
    };
    const uint8_t rows[] = {
        static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
        static_cast<uint8_t>(row_end >> 8), static_cast<uint8_t>(row_end),
    };
    lcd_command_data(0x2A, columns, sizeof(columns));
    lcd_command_data(0x2B, rows, sizeof(rows));
    lcd_command(0x2C);

    std::array<uint16_t, kDisplayWidth * 8> pixels;
    const uint16_t wire_color = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    pixels.fill(wire_color);
    for (int row = 0; row < height; row += 8) {
        const int rows_this_transfer = (height - row < 8) ? height - row : 8;
        lcd_data(pixels.data(), width * rows_this_transfer * sizeof(uint16_t));
    }
}

void lcd_fill(uint16_t rgb565) {
    lcd_fill_rect(0, 0, kDisplayWidth, kDisplayHeight, rgb565);
}

void lcd_show_color_bars() {
    lcd_fill_rect(0, 0, 32, kDisplayHeight, 0xF800);
    lcd_fill_rect(32, 0, 32, kDisplayHeight, 0x07E0);
    lcd_fill_rect(64, 0, 32, kDisplayHeight, 0x001F);
    lcd_fill_rect(96, 0, 32, kDisplayHeight, 0xFFFF);
}

void lcd_show_button_state(bool left_pressed, bool right_pressed, bool codec_found) {
    if (left_pressed && right_pressed) {
        lcd_fill_rect(0, 0, kDisplayWidth, kDisplayHeight / 2, 0xFFE0);
        lcd_fill_rect(0, kDisplayHeight / 2, kDisplayWidth, kDisplayHeight / 2, 0xF81F);
    } else if (right_pressed) {
        lcd_show_color_bars();
    } else if (left_pressed) {
        lcd_fill(0xFFE0);
    } else {
        lcd_fill(codec_found ? 0x07E0 : 0xF800);
    }
}

void lcd_show_sleep_selection(uint8_t timeout_minutes) {
    // The diagnostic image has no text renderer. Use its established color bars:
    // yellow=1 minute, cyan=5 minutes, magenta=never, green=saved confirmation.
    lcd_fill(timeout_minutes == 1 ? 0xFFE0 : (timeout_minutes == 5 ? 0x07FF : 0xF81F));
}

void lcd_show_shutdown_countdown(uint8_t seconds_remaining) {
    // Text-free diagnostic convention: yellow=3, gold=2, red=1, magenta=goodbye.
    const uint16_t color = seconds_remaining == 3 ? 0xFFE0
                           : seconds_remaining == 2 ? 0xFD20
                           : seconds_remaining == 1 ? 0xF800
                                                    : 0xF81F;
    lcd_fill(color);
}

void lcd_show_battery_indicator() {
    constexpr int x = 8;
    constexpr int y = 116;
    constexpr int width = 112;
    constexpr int height = 8;
    lcd_fill_rect(x, y, width, height, 0x4208);
    lcd_fill_rect(x + 1, y + 1, width - 2, height - 2, 0x0000);
    if (!battery_ready || (!battery_bar_visible && battery_reading.charging &&
                           battery_reading.percent < 50)) {
        return;
    }

    const int fill_width = ((width - 2) * battery_reading.percent) / 100;
    const uint16_t color = battery_reading.percent < 10
                               ? 0xF800
                               : (battery_reading.percent < 50 ? 0xFFE0 : 0x07E0);
    if (fill_width > 0) {
        lcd_fill_rect(x + 1, y + 1, fill_width, height - 2, color);
    }
    if (battery_reading.charging) {
        lcd_fill_rect(x + width - 5, y + 2, 3, height - 4, 0xFFFF);
    }
}

void update_battery(bool force = false) {
    if (!battery_ready) return;
    const TickType_t now = xTaskGetTickCount();
    const bool charging_changed = aipi_battery_is_charging() != battery_reading.charging;
    if (!force && !charging_changed && now - last_battery_read < pdMS_TO_TICKS(30000)) {
        return;
    }

    AipiBatteryReading reading = {};
    const esp_err_t result = aipi_battery_read(&reading);
    last_battery_read = now;
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "battery read failed: %s", esp_err_to_name(result));
        return;
    }

    battery_reading = reading;
    battery_bar_visible = true;
    last_battery_blink = now;
    ESP_LOGI(kTag, "battery=%lu.%03luV percent=%u charging=%d adc_calibrated=%d",
             static_cast<unsigned long>(reading.millivolts / 1000),
             static_cast<unsigned long>(reading.millivolts % 1000), reading.percent,
             reading.charging ? 1 : 0, reading.calibrated ? 1 : 0);
    if (!screen_overlay_active) {
        lcd_show_battery_indicator();
    }
}

void tick_battery_indicator() {
    update_battery();
    if (!battery_ready || !battery_reading.charging || battery_reading.percent >= 50) return;
    const TickType_t now = xTaskGetTickCount();
    if (now - last_battery_blink >= pdMS_TO_TICKS(600)) {
        battery_bar_visible = !battery_bar_visible;
        last_battery_blink = now;
        if (!screen_overlay_active) {
            lcd_show_battery_indicator();
        }
    }
}

bool screen_wake() {
    const bool woke_screen = !screen_backlight_on;
    screen_last_activity = xTaskGetTickCount();
    if (!screen_backlight_on) {
        gpio_set_level(kBacklight, 1);
        screen_backlight_on = true;
    }
    return woke_screen;
}

void screen_timeout_tick(bool codec_found) {
    const TickType_t now = xTaskGetTickCount();
    if (screen_confirmation_until != 0 && now >= screen_confirmation_until) {
        screen_confirmation_until = 0;
        screen_overlay_active = false;
        lcd_fill(codec_found ? 0x07E0 : 0xF800);
        lcd_show_battery_indicator();
    }

    const uint8_t timeout_minutes = kScreenTimeoutMinutes[screen_timeout_index];
    if (timeout_minutes == 0 || !screen_backlight_on) return;
    if (now - screen_last_activity < pdMS_TO_TICKS(timeout_minutes * 60000UL)) return;
    gpio_set_level(kBacklight, 0);
    screen_backlight_on = false;
}

#if AIPI_ENABLE_LEFT_SHUTDOWN
[[noreturn]] void shutdown_to_deep_sleep() {
    ESP_LOGI(kTag, "GPIO1 shutdown confirmed; entering deep sleep");
    ESP_ERROR_CHECK(esp_wifi_stop());
    gpio_set_level(kBacklight, 0);
    screen_backlight_on = false;
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << kAipiLeftButtonPin,
                                                  ESP_EXT1_WAKEUP_ANY_LOW));
    vTaskDelay(pdMS_TO_TICKS(25));
    esp_deep_sleep_start();
    __builtin_unreachable();
}
#endif

bool probe_es8311() {
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    const esp_err_t result = i2c_master_probe(bus, kEs8311Address, 100);
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
    return result == ESP_OK;
}

}  // namespace

extern "C" void app_main() {
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(kTag, "AIPI-Screen hardware test");
    ESP_LOGI(kTag, "chip model=%d cores=%d revision=%d flash=%luMB", chip.model, chip.cores,
             chip.revision, static_cast<unsigned long>(flash_size / (1024 * 1024)));

    const esp_err_t battery_result = aipi_battery_init();
    battery_ready = battery_result == ESP_OK;
    if (!battery_ready) {
        ESP_LOGE(kTag, "battery setup failed: %s", esp_err_to_name(battery_result));
    }
    ESP_ERROR_CHECK(aipi_controls_init());
    ESP_LOGI(kTag, "button raw levels at boot: GPIO1=%d GPIO42=%d (idle=1 pressed=0)",
             aipi_controls_raw_level(kAipiLeftButtonPin),
             aipi_controls_raw_level(kAipiRightButtonPin));
    const bool codec_found = probe_es8311();
    ESP_LOGI(kTag, "ES8311 at 0x18: %s", codec_found ? "PASS" : "FAIL");
    const esp_err_t audio_result = codec_found ? aipi_audio_init() : ESP_ERR_NOT_FOUND;
    ESP_LOGI(kTag, "ES8311 speaker playback: %s", esp_err_to_name(audio_result));

    lcd_init();
    lcd_fill(codec_found ? 0x07E0 : 0xF800);
    update_battery(true);
    lcd_show_battery_indicator();
    screen_last_activity = xTaskGetTickCount();
    ESP_LOGI(kTag, "LCD initialized; green means codec found, red means codec missing");
    ESP_LOGI(kTag, "buttons: GPIO1=yellow GPIO42=RGBW both=yellow/magenta");

    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(wifi_portal_start());

    while (true) {
        const AipiControlsState controls = aipi_controls_poll();
        if (controls.left_changed) {
            ESP_LOGI(kTag, "GPIO1 left: %s", controls.left_pressed ? "PRESSED" : "RELEASED");
#if AIPI_ENABLE_LEFT_SHUTDOWN
            if (controls.left_pressed) {
                left_wake_only = screen_wake();
                left_pressed_at = xTaskGetTickCount();
                left_shutdown_armed = false;
                left_shutdown_countdown_active = false;
                left_shutdown_countdown_shown = 0xff;
            } else if (left_wake_only) {
                left_wake_only = false;
                ESP_LOGI(kTag, "GPIO1 consumed as screen wake");
            } else if (left_shutdown_armed) {
                shutdown_to_deep_sleep();
            } else {
                if (left_shutdown_countdown_active) {
                    ESP_LOGI(kTag, "GPIO1 shutdown countdown cancelled");
                }
                left_shutdown_countdown_active = false;
                screen_overlay_active = false;
                lcd_fill(codec_found ? 0x07E0 : 0xF800);
                lcd_show_battery_indicator();
            }
#else
            if (controls.left_pressed) left_wake_only = screen_wake();
            else if (left_wake_only) {
                left_wake_only = false;
                ESP_LOGI(kTag, "GPIO1 consumed as screen wake");
            }
#endif
        }
        if (controls.right_changed) {
            ESP_LOGI(kTag, "GPIO42 right: %s", controls.right_pressed ? "PRESSED" : "RELEASED");
            if (controls.right_pressed) {
                right_wake_only = screen_wake();
                right_pressed_at = xTaskGetTickCount();
                right_next_cycle_at = 0;
                right_long_press_active = false;
            } else if (right_wake_only) {
                right_wake_only = false;
                ESP_LOGI(kTag, "GPIO42 consumed as screen wake");
            } else if (right_long_press_active) {
                screen_wake();
                right_long_press_active = false;
                screen_overlay_active = true;
                screen_confirmation_until = xTaskGetTickCount() + kScreenSaveConfirmationTicks;
                lcd_fill(0x07E0);
                ESP_LOGI(kTag, "screen sleep saved=%s",
                         kScreenTimeoutMinutes[screen_timeout_index] == 0 ? "never" :
                         (kScreenTimeoutMinutes[screen_timeout_index] == 1 ? "1_min" : "5_min"));
            } else if (aipi_audio_ready()) {
                audio_level_index = (audio_level_index + 1) %
                    (sizeof(kAudioLevels) / sizeof(kAudioLevels[0]));
                const uint8_t volume = kAudioLevels[audio_level_index];
                ESP_LOGI(kTag, "speaker sample volume=%u%%", volume);
                const esp_err_t sample_result = aipi_audio_play_volume_sample(volume);
                if (sample_result != ESP_OK) {
                    ESP_LOGE(kTag, "speaker sample failed: %s", esp_err_to_name(sample_result));
                }
            }
        }
        const TickType_t now = xTaskGetTickCount();
#if AIPI_ENABLE_LEFT_SHUTDOWN
        if (controls.left_pressed && !left_wake_only && !left_shutdown_armed) {
            if (!left_shutdown_countdown_active && now - left_pressed_at >= kShutdownHoldTicks) {
                left_shutdown_countdown_active = true;
                left_shutdown_countdown_started_at = now;
                left_shutdown_countdown_shown = 3;
                screen_overlay_active = true;
                lcd_show_shutdown_countdown(3);
                ESP_LOGI(kTag, "GPIO1 shutdown countdown started");
            }
            if (left_shutdown_countdown_active) {
                const TickType_t elapsed = now - left_shutdown_countdown_started_at;
                const uint8_t remaining = elapsed >= kShutdownCountdownTicks
                                              ? 0
                                              : static_cast<uint8_t>(3 - (elapsed / pdMS_TO_TICKS(1000)));
                if (remaining != left_shutdown_countdown_shown) {
                    left_shutdown_countdown_shown = remaining;
                    lcd_show_shutdown_countdown(remaining);
                }
                if (elapsed >= kShutdownCountdownTicks) {
                    left_shutdown_armed = true;
                    ESP_LOGI(kTag, "GPIO1 shutdown armed; release to confirm");
                }
            }
        }
#endif
        if (controls.right_pressed && !right_wake_only && !right_long_press_active &&
            now - right_pressed_at >= kScreenTimeoutHoldTicks) {
            screen_timeout_index = (screen_timeout_index + 1) %
                                   (sizeof(kScreenTimeoutMinutes) / sizeof(kScreenTimeoutMinutes[0]));
            right_long_press_active = true;
            right_next_cycle_at = now + kScreenTimeoutCycleTicks;
            screen_wake();
            screen_overlay_active = true;
            lcd_show_sleep_selection(kScreenTimeoutMinutes[screen_timeout_index]);
            ESP_LOGI(kTag, "screen sleep selection=%s",
                     kScreenTimeoutMinutes[screen_timeout_index] == 0 ? "never" :
                     (kScreenTimeoutMinutes[screen_timeout_index] == 1 ? "1_min" : "5_min"));
        } else if (controls.right_pressed && !right_wake_only && right_long_press_active && now >= right_next_cycle_at) {
            screen_timeout_index = (screen_timeout_index + 1) %
                                   (sizeof(kScreenTimeoutMinutes) / sizeof(kScreenTimeoutMinutes[0]));
            right_next_cycle_at = now + kScreenTimeoutCycleTicks;
            screen_wake();
            lcd_show_sleep_selection(kScreenTimeoutMinutes[screen_timeout_index]);
            ESP_LOGI(kTag, "screen sleep selection=%s",
                     kScreenTimeoutMinutes[screen_timeout_index] == 0 ? "never" :
                     (kScreenTimeoutMinutes[screen_timeout_index] == 1 ? "1_min" : "5_min"));
        }
        if ((controls.left_changed || controls.right_changed) && !screen_overlay_active) {
            lcd_show_button_state(controls.left_pressed, controls.right_pressed, codec_found);
            lcd_show_battery_indicator();
        }
        tick_battery_indicator();
        screen_timeout_tick(codec_found);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
