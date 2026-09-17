#pragma once
#include "cJSON.h"
// Owned by the response worker; reset only after its shutdown barrier.
void delegation_reset(void);
// Returns a caller-owned array of completed function items, or NULL.
cJSON *delegation_event(const cJSON *envelope);
unsigned delegation_selftest(void);
