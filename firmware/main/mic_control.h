#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
void mic_control_init(void);
void mic_control_process(int16_t *clean,const int16_t *raw,size_t count);
void mic_control_tick(void);
void mic_control_cancel(void);
bool mic_control_active(void);
esp_err_t mic_control_start(void);
cJSON *mic_control_status(void);
unsigned mic_control_selftest(void);
bool audio_is_playing(void);
unsigned audio_output_generation(void);
