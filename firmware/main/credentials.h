#pragma once
#include <stdbool.h>
#include "esp_err.h"
void credentials_init(void);
bool credentials_saved(void);
bool credentials_secure_storage(void);
bool credentials_present(void);
void credentials_copy(char out[513]);
esp_err_t credentials_set(const char *key);
esp_err_t credentials_provision(void);
