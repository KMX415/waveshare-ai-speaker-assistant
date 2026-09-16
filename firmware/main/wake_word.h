#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
void wake_word_init(void);
void wake_word_feed(const int16_t *samples,size_t count);
esp_err_t wake_word_configure(bool enabled,int phrase,int sensitivity);
esp_err_t wake_word_test(bool enabled);
bool wake_word_testing(void);
bool wake_word_listening(void);
bool wake_word_pause_for_voice(void);
void wake_word_resume_after_voice(void);
cJSON *wake_word_status(void);
