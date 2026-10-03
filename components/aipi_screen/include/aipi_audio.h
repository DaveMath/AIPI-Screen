#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t aipi_audio_init();
bool aipi_audio_ready();
esp_err_t aipi_audio_play_volume_sample(uint8_t volume_percent);
