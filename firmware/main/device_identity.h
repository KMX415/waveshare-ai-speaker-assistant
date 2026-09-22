#pragma once
#include "cJSON.h"
#include "esp_err.h"
void device_identity_init(void);
cJSON *device_identity_status(void);
esp_err_t device_identity_set_room(const char *room);
