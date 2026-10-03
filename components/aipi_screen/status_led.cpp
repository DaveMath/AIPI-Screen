#include "status_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr char kTag[] = "status_led";
constexpr gpio_num_t kLedPin = GPIO_NUM_46;
constexpr uint32_t kResolutionHz = 10 * 1000 * 1000;

rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;

uint8_t scale(uint8_t channel_value, uint8_t brightness) {
    return static_cast<uint8_t>((static_cast<uint16_t>(channel_value) * brightness + 127) / 255);
}

}  // namespace

esp_err_t status_led_init() {
    rmt_tx_channel_config_t channel_config = {};
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.gpio_num = kLedPin;
    channel_config.mem_block_symbols = 64;
    channel_config.resolution_hz = kResolutionHz;
    channel_config.trans_queue_depth = 2;
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &channel), kTag, "create RMT channel");

    rmt_bytes_encoder_config_t encoder_config = {};
    encoder_config.bit0.level0 = 1;
    encoder_config.bit0.duration0 = 3;
    encoder_config.bit0.level1 = 0;
    encoder_config.bit0.duration1 = 9;
    encoder_config.bit1.level0 = 1;
    encoder_config.bit1.duration0 = 9;
    encoder_config.bit1.level1 = 0;
    encoder_config.bit1.duration1 = 3;
    encoder_config.flags.msb_first = 1;
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&encoder_config, &encoder), kTag, "create LED encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(channel), kTag, "enable RMT channel");
    return status_led_set_rgb(0, 0, 255, 38);
}

esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness) {
    if (channel == nullptr || encoder == nullptr) return ESP_ERR_INVALID_STATE;

    // The AIPI Lite's single WS2812 uses GRB wire order.
    const uint8_t pixels[] = {
        scale(green, brightness),
        scale(red, brightness),
        scale(blue, brightness),
    };
    rmt_transmit_config_t transmit_config = {};
    transmit_config.loop_count = 0;
    transmit_config.flags.eot_level = 0;
    ESP_RETURN_ON_ERROR(rmt_transmit(channel, encoder, pixels, sizeof(pixels), &transmit_config),
                        kTag, "transmit LED color");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(channel, pdMS_TO_TICKS(50)), kTag, "wait for LED");
    esp_rom_delay_us(80);
    return ESP_OK;
}
