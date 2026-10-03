#include "aipi_audio.h"

#include <algorithm>
#include <array>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint8_t kCodecAddress = 0x18;
constexpr gpio_num_t kI2cSda = GPIO_NUM_5;
constexpr gpio_num_t kI2cScl = GPIO_NUM_4;
constexpr gpio_num_t kSpeakerEnable = GPIO_NUM_9;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_14;
constexpr gpio_num_t kI2sWordSelect = GPIO_NUM_12;
constexpr gpio_num_t kI2sDataOut = GPIO_NUM_11;
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kDacMuteMask = 0x60;

i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t codec = nullptr;
i2s_chan_handle_t tx_channel = nullptr;
bool ready = false;

esp_err_t write_register(uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(codec, bytes, sizeof(bytes), 100);
}

esp_err_t read_register(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(codec, &reg, 1, value, 1, 100);
}

esp_err_t set_muted(bool muted) {
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(read_register(0x31, &value), "aipi_audio", "read DAC mute");
    value = muted ? (value | kDacMuteMask) : (value & ~kDacMuteMask);
    return write_register(0x31, value);
}

int16_t amplitude_for_volume(uint8_t volume_percent) {
    const uint8_t bounded = std::min<uint8_t>(volume_percent, 100);
    return static_cast<int16_t>((14000U * bounded) / 100U);
}

esp_err_t write_frames(const int16_t* frames, size_t frame_count) {
    size_t bytes_written = 0;
    const size_t bytes = frame_count * 2 * sizeof(int16_t);
    ESP_RETURN_ON_ERROR(
        i2s_channel_write(tx_channel, frames, bytes, &bytes_written, portMAX_DELAY),
        "aipi_audio", "write I2S frames");
    return bytes_written == bytes ? ESP_OK : ESP_FAIL;
}

esp_err_t write_silence(uint16_t duration_ms) {
    std::array<int16_t, 128 * 2> frames = {};
    size_t remaining = (kSampleRate * duration_ms) / 1000;
    while (remaining > 0) {
        const size_t count = std::min<size_t>(remaining, 128);
        ESP_RETURN_ON_ERROR(write_frames(frames.data(), count), "aipi_audio", "write silence");
        remaining -= count;
    }
    return ESP_OK;
}

esp_err_t write_tone(uint16_t frequency, uint16_t duration_ms, uint8_t volume_percent) {
    std::array<int16_t, 128 * 2> frames;
    const int16_t amplitude = amplitude_for_volume(volume_percent);
    const size_t sample_count = (kSampleRate * duration_ms) / 1000;
    const size_t period = std::max<size_t>(2, kSampleRate / frequency);
    const size_t half_period = std::max<size_t>(1, period / 2);
    size_t generated = 0;
    while (generated < sample_count) {
        const size_t count = std::min<size_t>(sample_count - generated, 128);
        for (size_t i = 0; i < count; ++i) {
            const int16_t sample = ((generated + i) % period) < half_period ? amplitude : -amplitude;
            frames[i * 2] = sample;
            frames[i * 2 + 1] = sample;
        }
        ESP_RETURN_ON_ERROR(write_frames(frames.data(), count), "aipi_audio", "write tone");
        generated += count;
    }
    return ESP_OK;
}

esp_err_t begin_playback() {
    ESP_RETURN_ON_ERROR(set_muted(false), "aipi_audio", "unmute DAC");
    ESP_RETURN_ON_ERROR(gpio_set_level(kSpeakerEnable, 1), "aipi_audio", "enable amplifier");
    vTaskDelay(pdMS_TO_TICKS(8));
    return write_silence(12);
}

esp_err_t end_playback(esp_err_t playback_result) {
    const esp_err_t tail_result = write_silence(40);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(kSpeakerEnable, 0);
    const esp_err_t mute_result = set_muted(true);
    if (playback_result != ESP_OK) return playback_result;
    if (tail_result != ESP_OK) return tail_result;
    return mute_result;
}

esp_err_t play_two_tone(uint16_t first_hz, uint16_t second_hz, uint16_t note_ms,
                        uint16_t gap_ms, uint8_t volume_percent) {
    ESP_RETURN_ON_ERROR(begin_playback(), "aipi_audio", "begin playback");
    esp_err_t result = write_tone(first_hz, note_ms, volume_percent);
    if (result == ESP_OK) result = write_silence(gap_ms);
    if (result == ESP_OK) result = write_tone(second_hz, note_ms, volume_percent);
    return end_playback(result);
}

}  // namespace

esp_err_t aipi_audio_init() {
    gpio_config_t amp = {};
    amp.pin_bit_mask = 1ULL << kSpeakerEnable;
    amp.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&amp), "aipi_audio", "configure amplifier");
    ESP_RETURN_ON_ERROR(gpio_set_level(kSpeakerEnable, 0), "aipi_audio", "mute amplifier");

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus), "aipi_audio", "create I2C bus");

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kCodecAddress;
    device_config.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &device_config, &codec),
                        "aipi_audio", "add ES8311");
    ESP_RETURN_ON_ERROR(i2c_master_probe(i2c_bus, kCodecAddress, 100),
                        "aipi_audio", "probe ES8311");

    static constexpr uint8_t sequence[][2] = {
        {0x00, 0x1F}, {0x00, 0x00},
        {0x01, 0x9F}, {0x02, 0x10}, {0x03, 0x10}, {0x04, 0x20},
        {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
        {0x09, 0x0C}, {0x0A, 0x0C},
        {0x32, 0xBF}, {0x12, 0x00}, {0x13, 0x10},
        {0x31, kDacMuteMask}, {0x37, 0x08},
        {0x0D, 0x01}, {0x0E, 0x02}, {0x00, 0x80},
    };
    for (const auto& entry : sequence) {
        ESP_RETURN_ON_ERROR(write_register(entry[0], entry[1]), "aipi_audio", "initialize ES8311");
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &tx_channel, nullptr),
                        "aipi_audio", "create I2S channel");
    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = kI2sBclk,
            .ws = kI2sWordSelect,
            .dout = kI2sDataOut,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_channel, &standard_config),
                        "aipi_audio", "configure I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_channel), "aipi_audio", "enable I2S");
    ready = true;
    return ESP_OK;
}

bool aipi_audio_ready() { return ready; }

esp_err_t aipi_audio_play_volume_sample(uint8_t volume_percent) {
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (volume_percent == 0) return ESP_OK;
    if (volume_percent <= 10) {
        ESP_RETURN_ON_ERROR(begin_playback(), "aipi_audio", "begin 10 percent sample");
        return end_playback(write_tone(900, 180, volume_percent));
    }
    if (volume_percent <= 50) return play_two_tone(1500, 1500, 75, 70, volume_percent);
    return play_two_tone(2000, 2800, 110, 40, volume_percent);
}
