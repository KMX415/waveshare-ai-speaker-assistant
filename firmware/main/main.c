// USB packets: 'HV', type (1 command, 2 PCM, 3 status), uint16 LE length, payload.
// Console logs go to UART0, never into the USB packet stream.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include "board_audio.h"
#include "setup_portal.h"
#include "credentials.h"
#include "esp_system.h"
#include "live_voice.h"
#include "wake_word.h"
#include "activity_leds.h"
#include "hangup_phrase.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

typedef struct { int16_t samples[AUDIO_SAMPLES]; } audio_frame;
enum { PLAYBACK_FRAMES=32, PLAYBACK_PREFILL=8, PLAYBACK_WAIT_MS=160 };
static QueueHandle_t playback;
static atomic_uint playback_epoch,playback_peak,playback_gaps,last_enqueue_ms;
static atomic_uint playback_chunks,playback_samples,playback_partial_flushes;
static atomic_uint playback_chunk_min,playback_chunk_max,playback_arrival_max_ms;
static atomic_uint playback_writes,playback_write_max_ms,playback_slow_writes;
static SemaphoreHandle_t tx_lock;
static atomic_bool streaming;
static atomic_int tone_frames;
static int64_t last_command;
static SemaphoreHandle_t playback_lock;
static audio_frame queued_frame;
static size_t queued_samples;
bool audio_usb_active(void) { return atomic_load(&streaming); }
void audio_tone(void) { atomic_store(&tone_frames,15); }
void audio_clear(void) {
    xSemaphoreTake(playback_lock,portMAX_DELAY);xQueueReset(playback);queued_samples=0;atomic_fetch_add(&playback_epoch,1);xSemaphoreGive(playback_lock);
}
bool audio_enqueue(const int16_t *samples,size_t count) {
    bool ok=true;xSemaphoreTake(playback_lock,portMAX_DELAY);
    unsigned now=(unsigned)(esp_timer_get_time()/1000);
    unsigned previous=atomic_exchange(&last_enqueue_ms,now);
    unsigned interval=now-previous;
    if(previous && interval<1000 && interval>atomic_load(&playback_arrival_max_ms))atomic_store(&playback_arrival_max_ms,interval);
    atomic_fetch_add(&playback_chunks,1);atomic_fetch_add(&playback_samples,count);
    if(!atomic_load(&playback_chunk_min)||count<atomic_load(&playback_chunk_min))atomic_store(&playback_chunk_min,count);
    if(count>atomic_load(&playback_chunk_max))atomic_store(&playback_chunk_max,count);
    while(count) {
        size_t take=AUDIO_SAMPLES-queued_samples;if(take>count)take=count;
        memcpy(queued_frame.samples+queued_samples,samples,take*2);queued_samples+=take;samples+=take;count-=take;
        if(queued_samples==AUDIO_SAMPLES) {
            queued_samples=0;
            if(xQueueSend(playback,&queued_frame,pdMS_TO_TICKS(30))!=pdTRUE){ok=false;break;}
            unsigned depth=uxQueueMessagesWaiting(playback);
            if(depth>atomic_load(&playback_peak))atomic_store(&playback_peak,depth);
        }
    }
    xSemaphoreGive(playback_lock);return ok;
}
static void button_task(void *unused) {
    gpio_config_t config={.pin_bit_mask=1ULL<<0,.mode=GPIO_MODE_INPUT,.pull_up_en=true};
    ESP_ERROR_CHECK(gpio_config(&config));
    unsigned held=0;
    for(;;) {
        if(!gpio_get_level(0))held++;
        else if(held) {
            if(held>=50){live_voice_stop();setup_portal_enable();audio_tone();}
            else if(held>=2){if(live_voice_active())live_voice_stop();else live_voice_start();}
            held=0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void packet(uint8_t type, const void *data, uint16_t length) {
    uint8_t header[] = {'H','V',type,length & 255,length >> 8};
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    int h = usb_serial_jtag_write_bytes(header, sizeof(header), pdMS_TO_TICKS(100));
    int n = h == sizeof(header) ? usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(100)) : -1;
    if (n != length) atomic_store(&streaming, false);
    xSemaphoreGive(tx_lock);
}
static void status(const char *text) { packet(3, text, strlen(text)); }
static void audio_settings(void) {
    char message[64];
    snprintf(message, sizeof(message), "{\"type\":\"audio_settings\",\"volume\":%d}", setup_portal_volume());
    status(message);
}
static void device_state(void) {
    char text[512];
    esp_netif_ip_info_t ip={0};esp_netif_t *station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(station)esp_netif_get_ip_info(station,&ip);
    snprintf(text,sizeof(text),"{\"type\":\"device_state\",\"active\":%s,\"key_set\":%s,\"key_saved\":%s,\"secure_storage\":%s,\"wifi_connected\":%s,\"message\":\"%s\",\"finalized\":%s,\"seconds\":%d}",
        live_voice_active()?"true":"false",credentials_present()?"true":"false",credentials_saved()?"true":"false",
        credentials_secure_storage()?"true":"false",ip.ip.addr?"true":"false",live_voice_status(),live_voice_finalized()?"true":"false",live_voice_usage_seconds());
    status(text);
}

static void speaker_task(void *unused) {
    audio_frame frame;
    unsigned phase = 0;
    unsigned epoch=0,first_wait=0,gap_started=0;bool playing=false;
    for (;;) {
        unsigned now=(unsigned)(esp_timer_get_time()/1000),current_epoch=atomic_load(&playback_epoch);
        if(epoch!=current_epoch){epoch=current_epoch;playing=false;first_wait=0;gap_started=0;}
        // Flush a final short PCM frame instead of leaving the last syllable stranded.
        if(xSemaphoreTake(playback_lock,0)==pdTRUE){
            if(queued_samples && now-atomic_load(&last_enqueue_ms)>=240){
                memset(queued_frame.samples+queued_samples,0,(AUDIO_SAMPLES-queued_samples)*sizeof(int16_t));
                if(xQueueSend(playback,&queued_frame,0)==pdTRUE){queued_samples=0;atomic_fetch_add(&playback_partial_flushes,1);}
            }
            xSemaphoreGive(playback_lock);
        }
        memset(&frame,0,sizeof(frame));
        if (atomic_load(&tone_frames) > 0) {
            for (int i = 0; i < AUDIO_SAMPLES; ++i)
                frame.samples[i] = (int16_t)(1800 * sinf(2 * 3.14159265f * 440 * phase++ / 16000));
            atomic_fetch_sub(&tone_frames, 1);
        } else {
            unsigned depth=uxQueueMessagesWaiting(playback);
            if(!playing && depth){
                if(!first_wait)first_wait=now;
                if(depth>=PLAYBACK_PREFILL || now-first_wait>=PLAYBACK_WAIT_MS){
                    playing=true;first_wait=0;
                    // A brief refill gap is a useful symptom, not proof of network loss.
                    if(gap_started && now-gap_started<=300)atomic_fetch_add(&playback_gaps,1);
                    gap_started=0;
                }
            }
            if(playing && xQueueReceive(playback,&frame,0)!=pdTRUE){playing=false;gap_started=now;}
        }
        int64_t write_started=esp_timer_get_time();
        ESP_ERROR_CHECK(board_audio_write(frame.samples));
        unsigned write_ms=(unsigned)((esp_timer_get_time()-write_started)/1000);
        atomic_fetch_add(&playback_writes,1);
        if(write_ms>atomic_load(&playback_write_max_ms))atomic_store(&playback_write_max_ms,write_ms);
        if(write_ms>40)atomic_fetch_add(&playback_slow_writes,1);
        activity_leds_audio(frame.samples,AUDIO_SAMPLES);
    }
}
static void mic_task(void *unused) {
    int16_t raw[AUDIO_SAMPLES*4];
    aec_handle_t *aec = aec_create(16000, 4, 1, AEC_MODE_VOIP_HIGH_PERF);
    configASSERT(aec);
    int chunk = aec_get_chunksize(aec), filled = 0;
    int16_t *mic = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_8BIT);
    int16_t *reference = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_8BIT);
    int16_t *clean = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_8BIT);
    configASSERT(mic && reference && clean);
    unsigned count = 0;
    int peak1 = 0, peak2 = 0, ref_peak = 0;
    for (;;) {
        ESP_ERROR_CHECK(board_audio_read(raw));
        for (int i = 0; i < AUDIO_SAMPLES; ++i) {
            int r = abs(raw[4*i]), a = abs(raw[4*i+1]), b = abs(raw[4*i+3]);
            if (a > peak1) peak1 = a;
            if (b > peak2) peak2 = b;
            if (r > ref_peak) ref_peak = r;
            mic[filled] = raw[4*i+1];
            reference[filled++] = raw[4*i];
            if (filled == chunk) {
                aec_process(aec, mic, reference, clean);
                live_voice_feed(clean,chunk);
                wake_word_feed(clean,chunk);
                if (atomic_load(&streaming)) {
                    for (int offset = 0; offset < chunk; offset += AUDIO_SAMPLES) {
                        int samples = chunk - offset;
                        if (samples > AUDIO_SAMPLES) samples = AUDIO_SAMPLES;
                        packet(2, clean + offset, samples * sizeof(int16_t));
                    }
                }
                filled = 0;
            }
        }
        if (++count == 25) {
            char message[160];
            snprintf(message, sizeof(message), "{\"type\":\"levels\",\"mic1\":%d,\"mic2\":%d,\"reference\":%d}", peak1, peak2, ref_peak);
            if (!live_voice_active() && esp_timer_get_time()-last_command<3000000) packet(3, message, strlen(message));
            count = peak1 = peak2 = ref_peak = 0;
        }
    }
}
static bool read_exact(void *buffer, size_t length) {
    size_t done = 0;
    while (done < length) {
        int n = usb_serial_jtag_read_bytes((uint8_t *)buffer + done, length - done, pdMS_TO_TICKS(250));
        if (n <= 0) return false;
        done += n;
    }
    return true;
}
void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size = 4096, .tx_buffer_size = 4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    tx_lock = xSemaphoreCreateMutex();
    static StaticQueue_t playback_control;
    uint8_t *playback_storage=heap_caps_malloc(PLAYBACK_FRAMES*sizeof(audio_frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    configASSERT(playback_storage);
    playback = xQueueCreateStatic(PLAYBACK_FRAMES,sizeof(audio_frame),playback_storage,&playback_control);
    playback_lock=xSemaphoreCreateMutex();
    configASSERT(tx_lock && playback && playback_lock);
    ESP_ERROR_CHECK(board_audio_init());
    credentials_init();
    live_voice_init();
    setup_portal_init();
    wake_word_init();
    activity_leds_init();
    xTaskCreate(speaker_task, "speaker", 6144, NULL, 6, NULL);
    xTaskCreatePinnedToCore(mic_task, "microphone", 16384, NULL, 5, NULL, 1);
    xTaskCreate(button_task,"button",4096,NULL,2,NULL);
    uint8_t data[640], byte, previous = 0, header[3];
    audio_frame pending = {0};
    size_t pending_bytes = 0;
    for (;;) {
        if (!read_exact(&byte, 1)) {
            if (esp_timer_get_time() - last_command > 3000000) {
                atomic_store(&streaming, false);
                if(!live_voice_active())audio_clear();
                pending_bytes = 0;
            }
            previous = 0;
            continue;
        }
        if (previous != 'H' || byte != 'V') { previous = byte; continue; }
        previous = 0;
        if (!read_exact(header, 3)) continue;
        unsigned length = header[1] | (header[2] << 8);
        if (length > sizeof(data) || !read_exact(data, length)) continue;
        last_command = esp_timer_get_time();
        if (header[0] == 1 && length >= 1) {
            switch (data[0]) {
            case 'I': status("{\"type\":\"device\",\"firmware\":\"home-voice-standalone-0.6.1\",\"standalone\":true,\"aec\":true,\"sample_rate\":16000}"); audio_settings();device_state();break;
            case 'L': {
                char report[120];
                snprintf(report,sizeof(report),"{\"type\":\"led_status\",\"frames\":%u,\"audio_frames\":%u}",activity_leds_frames(),activity_leds_audio_frames());
                status(report);break;
            }
            case 'H': {
                char report[64];
                snprintf(report,sizeof(report),"{\"type\":\"portal_selftest\",\"passed_mask\":%lu}",(unsigned long)setup_portal_selftest());
                status(report);break;
            }
            case 'N': device_state();break;
            case 'U': {cJSON *j=live_voice_upload_status();char *s=cJSON_PrintUnformatted(j);if(s){status(s);free(s);}cJSON_Delete(j);break;}
            case 'B': {
                char report[640];snprintf(report,sizeof(report),"{\"type\":\"playback_status\",\"queued_frames\":%u,\"peak_frames\":%u,\"short_refill_gaps\":%u,\"prefill_ms\":%u,\"capacity_ms\":640,\"chunks\":%u,\"samples\":%u,\"chunk_min\":%u,\"chunk_max\":%u,\"arrival_max_ms\":%u,\"partial_flushes\":%u,\"writes\":%u,\"write_max_ms\":%u,\"slow_writes\":%u}",(unsigned)uxQueueMessagesWaiting(playback),atomic_load(&playback_peak),atomic_load(&playback_gaps),PLAYBACK_WAIT_MS,atomic_load(&playback_chunks),atomic_load(&playback_samples),atomic_load(&playback_chunk_min),atomic_load(&playback_chunk_max),atomic_load(&playback_arrival_max_ms),atomic_load(&playback_partial_flushes),atomic_load(&playback_writes),atomic_load(&playback_write_max_ms),atomic_load(&playback_slow_writes));status(report);break;
            }
            case 'F': {cJSON *j=live_voice_failure_status();char *s=cJSON_PrintUnformatted(j);if(s){status(s);free(s);}cJSON_Delete(j);break;}
            case 'J': {
                char report[120];snprintf(report,sizeof(report),"{\"type\":\"hangup_status\",\"selftest\":%s,\"hangups\":%u}",hangup_phrase_selftest()?"true":"false",live_voice_hangups());
                status(report);break;
            }
            case 'Q': {cJSON *j=wake_word_status();char *s=cJSON_PrintUnformatted(j);if(s){status(s);free(s);}cJSON_Delete(j);break;}
            case 'W': {
                esp_err_t err=ESP_ERR_INVALID_ARG;
                if(length==4 && data[1]<=1)err=wake_word_configure(data[1],data[2],data[3]);
                if(length==2 && (data[1]==2||data[1]==3))err=wake_word_test(data[1]==2);
                status(err==ESP_OK?"{\"type\":\"wake_saved\"}":"{\"type\":\"wake_error\"}");break;
            }
            case 'R':
                if(length==8 && !memcmp(data,"RRESTART",8) && !live_voice_active())esp_restart();
                break;
            case 'E': {
                const char consent[]="EENABLE-HARDWARE-KEY-STORAGE";
                if(length==sizeof(consent)-1 && !memcmp(data,consent,sizeof(consent)-1) && !live_voice_active()) {
                    esp_err_t err=credentials_provision();
                    status(err==ESP_OK?"{\"type\":\"storage_provisioned\"}":"{\"type\":\"storage_error\"}");
                    if(err==ESP_OK){vTaskDelay(pdMS_TO_TICKS(250));esp_restart();}
                }
                break;
            }
            case 'D': {
                char report[256];
                snprintf(report,sizeof(report),"{\"type\":\"voice_diagnostics\",\"input_frames\":%u,\"max_send_ms\":%u,\"queue_peak\":%u,\"socket_error\":%d,\"tls_error\":%d,\"internal_free\":%u,\"internal_largest\":%u}",
                    live_voice_input_frames(),live_voice_max_send_ms(),live_voice_input_queue_peak(),live_voice_socket_error(),live_voice_tls_error(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
                status(report);break;
            }
            case 'G': live_voice_start();device_state();break;
            case 'Z': live_voice_stop();device_state();break;
            case 'K': {
                char key[513]={0};
                if(length>1 && length<=513 && !live_voice_active()) {
                    memcpy(key,data+1,length-1);
                    status(credentials_set(key)==ESP_OK?"{\"type\":\"key_saved\"}":"{\"type\":\"key_error\"}");
                }
                memset(key,0,sizeof(key));memset(data,0,sizeof(data));device_state();break;
            }
            case 'V':
                if (length == 2 && setup_portal_set_volume(data[1]) == ESP_OK) audio_settings();
                else status("{\"type\":\"settings_error\",\"message\":\"Volume update failed\"}");
                break;
            case 'S': if(!live_voice_active()){pending_bytes = 0; audio_clear(); atomic_store(&streaming, true);} break;
            case 'X': pending_bytes = 0; atomic_store(&streaming, false); xQueueReset(playback); break;
            case 'T': atomic_store(&tone_frames, 15); break;
            case 'P': break; // Host heartbeat; USB cable loss stops capture within three seconds.
            case 'C': {
                setup_portal_enable();
                char settings[192];
                snprintf(settings,sizeof(settings),"{\"type\":\"setup\",\"ssid\":\"%s\",\"password\":\"%s\",\"url\":\"http://192.168.4.1/\"}",setup_portal_ssid(),setup_portal_password());
                status(settings);
                break;
            }
            }
        } else if (header[0] == 2 && length > 0 && length <= 640 && length % 2 == 0) {
            for (size_t offset = 0; offset < length;) {
                size_t take = sizeof(pending) - pending_bytes;
                if (take > length - offset) take = length - offset;
                memcpy((uint8_t *)&pending + pending_bytes, data + offset, take);
                offset += take; pending_bytes += take;
                if (pending_bytes == sizeof(pending)) {
                    pending_bytes = 0;
                    if (xQueueSend(playback, &pending, 0) != pdTRUE) {
                        atomic_store(&streaming, false);
                        xQueueReset(playback);
                        status("{\"type\":\"error\",\"message\":\"Speaker buffer overflow\"}");
                        break;
                    }
                }
            }
        }
    }
}
