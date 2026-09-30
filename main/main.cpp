#include <array>
#include <stdint.h>

#include "aipi_controls.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

namespace {

constexpr char kTag[] = "aipi_screen";

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

    ESP_ERROR_CHECK(aipi_controls_init());
    ESP_LOGI(kTag, "button raw levels at boot: GPIO1=%d GPIO42=%d (idle=1 pressed=0)",
             aipi_controls_raw_level(kAipiLeftButtonPin),
             aipi_controls_raw_level(kAipiRightButtonPin));
    const bool codec_found = probe_es8311();
    ESP_LOGI(kTag, "ES8311 at 0x18: %s", codec_found ? "PASS" : "FAIL");

    lcd_init();
    lcd_fill(codec_found ? 0x07E0 : 0xF800);
    ESP_LOGI(kTag, "LCD initialized; green means codec found, red means codec missing");
    ESP_LOGI(kTag, "buttons: GPIO1=yellow GPIO42=RGBW both=yellow/magenta");

    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(wifi_portal_start());

    while (true) {
        const AipiControlsState controls = aipi_controls_poll();
        if (controls.left_changed) {
            ESP_LOGI(kTag, "GPIO1 left: %s", controls.left_pressed ? "PRESSED" : "RELEASED");
        }
        if (controls.right_changed) {
            ESP_LOGI(kTag, "GPIO42 right: %s", controls.right_pressed ? "PRESSED" : "RELEASED");
        }
        if (controls.left_changed || controls.right_changed) {
            lcd_show_button_state(controls.left_pressed, controls.right_pressed, codec_found);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
