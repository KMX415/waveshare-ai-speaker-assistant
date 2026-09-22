#pragma once
#include <stdbool.h>
typedef enum { RUNTIME_IDLE, RUNTIME_VOICE, RUNTIME_UPDATE } runtime_owner;
bool runtime_acquire(runtime_owner owner);
void runtime_release(runtime_owner owner);
bool runtime_updating(void);
