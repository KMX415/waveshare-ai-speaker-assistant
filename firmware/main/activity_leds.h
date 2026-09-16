#pragma once
#include <stddef.h>
#include <stdint.h>
void activity_leds_init(void);
void activity_leds_wake(void);
void activity_leds_audio(const int16_t *samples, size_t count);
unsigned activity_leds_frames(void);
unsigned activity_leds_audio_frames(void);
