#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#define HOME_VOICE_RELEASE 702
#define HOME_VOICE_VERSION "0.7.0-dev3"
#define HOME_VOICE_BOARD "waveshare-esp32-s3-audio"
#define HOME_VOICE_ASSETS "esp-sr-2.1.5-wn9-jarvis-computer"
void firmware_update_init(void);
bool audio_healthy(void);
esp_err_t firmware_update_request(bool install);
esp_err_t firmware_update_preview(bool install);
cJSON *firmware_update_status(void);

unsigned firmware_update_selftest(void);
