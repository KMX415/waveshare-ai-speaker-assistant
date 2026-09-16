#pragma once
#include <stdint.h>
#include "esp_err.h"
#define AUDIO_SAMPLES 320
esp_err_t board_audio_init(void);
esp_err_t board_audio_read(int16_t raw[AUDIO_SAMPLES * 4]);
esp_err_t board_audio_write(const int16_t mono[AUDIO_SAMPLES]);
esp_err_t board_audio_volume(int volume);
esp_err_t board_audio_gain(int gain);
