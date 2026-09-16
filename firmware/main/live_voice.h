#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
void live_voice_init(void);
esp_err_t live_voice_start(void);
void live_voice_stop(void);
bool live_voice_active(void);
bool live_voice_ready(void);
bool live_voice_failed(void);
unsigned live_voice_hangups(void);
const char *live_voice_status(void);
bool live_voice_finalized(void);
int live_voice_usage_seconds(void);
unsigned live_voice_input_frames(void);
unsigned live_voice_max_send_ms(void);
unsigned live_voice_input_queue_peak(void);
int live_voice_socket_error(void);
int live_voice_tls_error(void);
void live_voice_feed(const int16_t *samples, size_t count);
bool audio_enqueue(const int16_t *samples,size_t count);
void audio_clear(void);
bool audio_usb_active(void);
void audio_tone(void);
