#include "aipi_battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

namespace {

constexpr gpio_num_t kChargeStatusPin = GPIO_NUM_8;
constexpr gpio_num_t kPowerHoldPin = GPIO_NUM_10;
constexpr adc_unit_t kBatteryAdcUnit = ADC_UNIT_1;
constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_1;
constexpr adc_atten_t kBatteryAttenuation = ADC_ATTEN_DB_12;
constexpr uint32_t kBatteryDividerMultiplierMilli = 2500;
constexpr int kSampleCount = 10;

adc_oneshot_unit_handle_t adc_handle = nullptr;
adc_cali_handle_t calibration_handle = nullptr;
bool calibration_enabled = false;

uint8_t interpolate_percent(uint32_t millivolts, uint32_t low_mv, uint8_t low_percent,
                            uint32_t high_mv, uint8_t high_percent) {
    return static_cast<uint8_t>(
        low_percent + ((millivolts - low_mv) * (high_percent - low_percent)) /
                          (high_mv - low_mv));
}

}  // namespace

uint8_t aipi_battery_percent_from_millivolts(uint32_t millivolts) {
    if (millivolts >= 4200) return 100;
    if (millivolts >= 4100) return interpolate_percent(millivolts, 4100, 90, 4200, 100);
    if (millivolts >= 3950) return interpolate_percent(millivolts, 3950, 70, 4100, 90);
    if (millivolts >= 3800) return interpolate_percent(millivolts, 3800, 50, 3950, 70);
    if (millivolts >= 3700) return interpolate_percent(millivolts, 3700, 30, 3800, 50);
    if (millivolts >= 3500) return interpolate_percent(millivolts, 3500, 10, 3700, 30);
    if (millivolts >= 3300) return interpolate_percent(millivolts, 3300, 0, 3500, 10);
    return 0;
}

esp_err_t aipi_battery_init() {
    gpio_config_t power_hold = {};
    power_hold.pin_bit_mask = 1ULL << kPowerHoldPin;
    power_hold.mode = GPIO_MODE_OUTPUT;
    esp_err_t result = gpio_config(&power_hold);
    if (result != ESP_OK) return result;
    result = gpio_set_level(kPowerHoldPin, 1);
    if (result != ESP_OK) return result;

    gpio_config_t charge_status = {};
    charge_status.pin_bit_mask = 1ULL << kChargeStatusPin;
    charge_status.mode = GPIO_MODE_INPUT;
    charge_status.pull_up_en = GPIO_PULLUP_ENABLE;
    charge_status.pull_down_en = GPIO_PULLDOWN_DISABLE;
    charge_status.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&charge_status);
    if (result != ESP_OK) return result;

    adc_oneshot_unit_init_cfg_t unit_config = {};
    unit_config.unit_id = kBatteryAdcUnit;
    result = adc_oneshot_new_unit(&unit_config, &adc_handle);
    if (result != ESP_OK) return result;

    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = kBatteryAttenuation;
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    result = adc_oneshot_config_channel(adc_handle, kBatteryAdcChannel, &channel_config);
    if (result != ESP_OK) return result;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration_config = {};
    calibration_config.unit_id = kBatteryAdcUnit;
    calibration_config.chan = kBatteryAdcChannel;
    calibration_config.atten = kBatteryAttenuation;
    calibration_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    calibration_enabled =
        adc_cali_create_scheme_curve_fitting(&calibration_config, &calibration_handle) == ESP_OK;
#endif
    return ESP_OK;
}

esp_err_t aipi_battery_read(AipiBatteryReading* reading) {
    if (reading == nullptr || adc_handle == nullptr) return ESP_ERR_INVALID_STATE;

    uint32_t pin_millivolts_sum = 0;
    for (int sample = 0; sample < kSampleCount; ++sample) {
        int raw = 0;
        esp_err_t result = adc_oneshot_read(adc_handle, kBatteryAdcChannel, &raw);
        if (result != ESP_OK) return result;

        int pin_millivolts = 0;
        if (calibration_enabled) {
            result = adc_cali_raw_to_voltage(calibration_handle, raw, &pin_millivolts);
            if (result != ESP_OK) return result;
        } else {
            pin_millivolts = (raw * 3100) / 4095;
        }
        pin_millivolts_sum += static_cast<uint32_t>(pin_millivolts);
    }

    const uint32_t average_pin_millivolts = pin_millivolts_sum / kSampleCount;
    reading->millivolts =
        (average_pin_millivolts * kBatteryDividerMultiplierMilli) / 1000;
    reading->percent = aipi_battery_percent_from_millivolts(reading->millivolts);
    reading->charging = aipi_battery_is_charging();
    reading->calibrated = calibration_enabled;
    return ESP_OK;
}

bool aipi_battery_is_charging() {
    return gpio_get_level(kChargeStatusPin) == 0;
}
