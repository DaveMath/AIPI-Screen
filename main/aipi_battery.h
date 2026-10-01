#pragma once

#include <stdint.h>

#include "esp_err.h"

struct AipiBatteryReading {
    uint32_t millivolts;
    uint8_t percent;
    bool charging;
    bool calibrated;
};

esp_err_t aipi_battery_init();
esp_err_t aipi_battery_read(AipiBatteryReading* reading);
bool aipi_battery_is_charging();
uint8_t aipi_battery_percent_from_millivolts(uint32_t millivolts);
