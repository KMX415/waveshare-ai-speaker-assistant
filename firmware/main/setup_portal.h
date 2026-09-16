#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
uint32_t setup_portal_selftest(void);
int setup_portal_volume(void);
esp_err_t setup_portal_set_volume(int value);
void setup_portal_init(void);
void setup_portal_enable(void);
const char *setup_portal_password(void);
const char *setup_portal_ssid(void);
