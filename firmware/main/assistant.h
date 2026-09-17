#pragma once
#include "cJSON.h"
#include "esp_err.h"

void assistant_init(void);
cJSON *assistant_settings(void);
esp_err_t assistant_save(const cJSON *settings);
char *assistant_start_event(void);
cJSON *assistant_execute(const char *name, const char *arguments);
