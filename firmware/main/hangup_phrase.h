#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {char text[96];unsigned used;uint32_t last_ms;} hangup_phrase_t;
void hangup_phrase_reset(hangup_phrase_t *p);
void hangup_phrase_feed(hangup_phrase_t *p,const char *delta,uint32_t now);
bool hangup_phrase_ready(const hangup_phrase_t *p,uint32_t now);
bool hangup_phrase_selftest(void);
