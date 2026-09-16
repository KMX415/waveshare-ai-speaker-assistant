#include "live_voice.h"
#include "credentials.h"
#include "wake_word.h"
#include "hangup_phrase.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_websocket_client.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#define MESSAGE_MAX (128*1024)
typedef struct { size_t bytes; int16_t samples[512]; } input_frame;
static QueueHandle_t inputs;
static QueueHandle_t responses;
static SemaphoreHandle_t responses_idle;
static atomic_bool busy, ready, stopping, connected, finalized, failed;
static atomic_uint activity_ms;
static atomic_int usage;
static atomic_uint input_frames_sent, max_send_ms, input_queue_peak;
static atomic_int socket_error, tls_error;
static char transport_failure[200];
int live_voice_socket_error(void) { return atomic_load(&socket_error); }
int live_voice_tls_error(void) { return atomic_load(&tls_error); }
static _Atomic(const char *) state="Ready";
static char *message;
static size_t message_used;
static int message_opcode;
static hangup_phrase_t hangup_phrase;
static atomic_uint hangup_count;
unsigned live_voice_hangups(void){return atomic_load(&hangup_count);}

bool live_voice_active(void) { return atomic_load(&busy); }
bool live_voice_ready(void) { return atomic_load(&ready) && !atomic_load(&stopping); }
bool live_voice_failed(void) { return atomic_load(&failed); }
const char *live_voice_status(void) { return atomic_load(&state); }
bool live_voice_finalized(void) { return atomic_load(&finalized); }
int live_voice_usage_seconds(void) { return atomic_load(&usage); }
unsigned live_voice_input_frames(void) { return atomic_load(&input_frames_sent); }
unsigned live_voice_max_send_ms(void) { return atomic_load(&max_send_ms); }
unsigned live_voice_input_queue_peak(void) { return atomic_load(&input_queue_peak); }
void live_voice_stop(void) { atomic_store(&stopping,true); }
static void fail(const char *text) {bool expected=false;if(atomic_compare_exchange_strong(&failed,&expected,true))state=text;live_voice_stop();}

static void parse_message(const char *text) {
    cJSON *root=cJSON_Parse(text);
    if(!root) {fail("Invalid voice response");return;}
    cJSON *type=cJSON_GetObjectItem(root,"type");
    if(!cJSON_IsString(type)) {cJSON_Delete(root);return;}
    if(!strcmp(type->valuestring,"session.started")) {
        hangup_phrase_reset(&hangup_phrase);
        atomic_store(&ready,true);state="Listening";
        audio_tone();
        atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
    } else if(!strcmp(type->valuestring,"session.output_audio.delta") && !atomic_load(&stopping)) {
        cJSON *delta=cJSON_GetObjectItem(root,"delta");
        if(cJSON_IsString(delta)) {
            size_t encoded=strlen(delta->valuestring), capacity=encoded/4*3+4, decoded=0;
            uint8_t *pcm=heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!pcm || mbedtls_base64_decode(pcm,capacity,&decoded,(uint8_t *)delta->valuestring,encoded)!=0 || decoded%2)
                fail("Invalid voice audio");
            else if(!audio_enqueue((int16_t *)pcm,decoded/2)) fail("Speaker backlog; press to try again");
            free(pcm);
        }
    } else if(!strcmp(type->valuestring,"session.input_transcript.delta") || !strcmp(type->valuestring,"session.output_transcript.delta")) {
        cJSON *delta=cJSON_GetObjectItem(root,"delta");
        if(cJSON_IsString(delta)&&delta->valuestring[0]) atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
        if(!strcmp(type->valuestring,"session.input_transcript.delta") && cJSON_IsString(delta) && live_voice_ready())
            hangup_phrase_feed(&hangup_phrase,delta->valuestring,(uint32_t)(esp_timer_get_time()/1000));
    } else if(!strcmp(type->valuestring,"session.closed")) {
        cJSON *seconds=cJSON_GetObjectItem(cJSON_GetObjectItem(root,"usage"),"seconds");
        if(cJSON_IsNumber(seconds)) atomic_store(&usage,seconds->valueint);
        atomic_store(&finalized,true);live_voice_stop();
    } else if(!strcmp(type->valuestring,"error")) fail("OpenAI rejected the session; check key and account access");
    cJSON_Delete(root);
}
static void response_task(void *unused) {
    for (;;) {
        char *text=NULL;
        if(xQueueReceive(responses,&text,pdMS_TO_TICKS(100))==pdTRUE){
            if(text) {parse_message(text);free(text);}
            else xSemaphoreGive(responses_idle);
        }
        if(live_voice_ready() && hangup_phrase_ready(&hangup_phrase,(uint32_t)(esp_timer_get_time()/1000))){
            atomic_fetch_add(&hangup_count,1);state="Hangup phrase heard; closing";
            live_voice_stop();audio_clear();
        }
    }
}
static void websocket_event(void *arg,esp_event_base_t base,int32_t id,void *raw) {
    esp_websocket_event_data_t *event=raw;
    if(id==WEBSOCKET_EVENT_ERROR && !atomic_load(&stopping) && event->data_ptr && event->data_len>0) {
        // SDK transport errors contain numeric diagnostics, never request headers.
        if(event->data_len>=14 && !memcmp(event->data_ptr,"esp_transport_",14)) {
            size_t n=event->data_len<sizeof(transport_failure)-1?event->data_len:sizeof(transport_failure)-1;
            memcpy(transport_failure,event->data_ptr,n);transport_failure[n]=0;
            fail(transport_failure);
        }
    }
    if(id==WEBSOCKET_EVENT_CONNECTED) {
        int fd=esp_transport_get_socket((esp_transport_handle_t)arg), enabled=1;
        if(fd<0 || setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled))!=0) {
            fail("Could not configure voice socket");return;
        }
        atomic_store(&connected,true);return;
    }
    if(id==WEBSOCKET_EVENT_DISCONNECTED && event->error_handle.error_type==WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        socket_error=event->error_handle.esp_transport_sock_errno;
        tls_error=event->error_handle.esp_tls_stack_err;
    }
    if(id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED) {
        if(!atomic_load(&stopping)) fail("Voice connection ended; press to reconnect");
        atomic_store(&connected,false);return;
    }
    if(id!=WEBSOCKET_EVENT_DATA || (event->op_code!=1 && event->op_code!=0)) return;
    if(event->payload_offset==0 && event->op_code==1) {message_used=0;message_opcode=1;}
    if(message_opcode!=1 || event->data_len<0 || message_used+event->data_len>=MESSAGE_MAX) {fail("Voice response exceeds buffer");return;}
    memcpy(message+message_used,event->data_ptr,event->data_len);message_used+=event->data_len;
    if(event->fin && event->payload_offset+event->data_len==event->payload_len) {
        message[message_used]=0;
        char *copy=heap_caps_malloc(message_used+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(copy)memcpy(copy,message,message_used+1);
        if(!copy || xQueueSend(responses,&copy,0)!=pdTRUE) {
            free(copy);fail("Voice response backlog; press to try again");
        }
        message_used=0;message_opcode=0;
    }
}
static const char start_event[]=
    "{\"type\":\"session.start\",\"session\":{\"model\":\"gpt-live-1\","
    "\"instructions\":\"You are a friendly home voice assistant. Speak naturally and briefly. Backchannel policy: use occasional unobtrusive acknowledgments. Interruption policy: yield when interrupted. Delegation policy: delegate factual questions and reasoning to the backend. No computer, home-control, weather or memory tools are connected.\","
    "\"audio\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":16000},\"output\":{\"voice\":\"marin\"}},"
    "\"delegation\":{\"type\":\"responses\",\"responses\":{\"model\":\"gpt-5.6-luna\",\"instructions\":\"Answer clearly and concisely for speech. You have no tools or current information. State uncertainty.\"}}}}";

static void session_task(void *unused) {
    esp_websocket_client_handle_t client=NULL;
    esp_transport_handle_t tls=NULL, ws=NULL;
    char key[513]={0},headers[560]={0};
    atomic_store(&ready,false);atomic_store(&connected,false);atomic_store(&finalized,false);atomic_store(&failed,false);atomic_store(&usage,-1);
    xQueueReset(inputs);audio_clear();state="Connecting Wi-Fi";
    input_frames_sent=0;max_send_ms=0;input_queue_peak=0;
    socket_error=0;tls_error=0;
    if(!wake_word_pause_for_voice()){fail("Wake detector did not release audio resources");goto cleanup;}
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    wifi_ap_record_t ap;
    int wait;
    for(wait=0;wait<150 && !atomic_load(&stopping);wait++) {
        esp_netif_ip_info_t ip={0};
        esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK && sta && esp_netif_get_ip_info(sta,&ip)==ESP_OK && ip.ip.addr)break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(wait==150) {fail("Wi-Fi unavailable; open device setup");goto cleanup;}
    state="Checking secure connection time";
    if(!esp_sntp_enabled()) {esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();}
    for(wait=0;wait<200 && time(NULL)<1767225600 && !atomic_load(&stopping);wait++)vTaskDelay(pdMS_TO_TICKS(100));
    if(time(NULL)<1767225600) {fail("Clock sync failed; check internet connection");goto cleanup;}
    if(atomic_load(&stopping))goto cleanup;
    credentials_copy(key);snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",key);memset(key,0,sizeof(key));
    tls=esp_transport_ssl_init();
    if(tls) {esp_transport_ssl_crt_bundle_attach(tls,esp_crt_bundle_attach);ws=esp_transport_ws_init(tls);}
    if(!ws){fail("Could not allocate voice transport");goto cleanup;}
    esp_transport_ws_set_path(ws,"/v1/live/sessions");
    if(esp_transport_ws_set_headers(ws,headers)!=ESP_OK){fail("Could not configure voice transport");goto cleanup;}
    esp_websocket_client_config_t config={.uri="wss://api.openai.com/v1/live/sessions",.port=443,.headers=headers,
        .crt_bundle_attach=esp_crt_bundle_attach,.disable_auto_reconnect=true,.buffer_size=4096,
        .task_stack=12288,.network_timeout_ms=5000,.ext_transport=ws};
    client=esp_websocket_client_init(&config);
    if(!client) {fail("Could not allocate voice connection");goto cleanup;}
    esp_websocket_register_events(client,WEBSOCKET_EVENT_ANY,websocket_event,tls);
    state="Connecting to OpenAI";
    if(esp_websocket_client_start(client)!=ESP_OK) {fail("Could not start voice connection");goto cleanup;}
    for(wait=0;wait<200&&!atomic_load(&connected)&&!atomic_load(&stopping);wait++)vTaskDelay(pdMS_TO_TICKS(100));
    if(!atomic_load(&connected)){fail("OpenAI connection failed; check key and internet");goto cleanup;}
    if(esp_websocket_client_send_text(client,start_event,strlen(start_event),pdMS_TO_TICKS(2000))<0){fail("Session startup failed");goto cleanup;}
    uint32_t began=esp_timer_get_time()/1000;
    while(!atomic_load(&stopping)) {
        uint32_t now=esp_timer_get_time()/1000;
        if(!atomic_load(&ready)&&now-began>20000){fail("Voice startup timed out");break;}
        if(now-began>600000 || (atomic_load(&ready)&&now-atomic_load(&activity_ms)>60000)){state="Conversation time limit reached";live_voice_stop();break;}
        input_frame frame;
        if(xQueueReceive(inputs,&frame,pdMS_TO_TICKS(20))==pdTRUE && atomic_load(&ready)) {
            char encoded[1370],json[1450];size_t length=0;
            mbedtls_base64_encode((uint8_t *)encoded,sizeof(encoded),&length,(uint8_t *)frame.samples,frame.bytes);encoded[length]=0;
            int size=snprintf(json,sizeof(json),"{\"type\":\"session.input_audio.append\",\"audio\":\"%s\"}",encoded);
            int64_t send_start=esp_timer_get_time();
            int sent=esp_websocket_client_send_text(client,json,size,pdMS_TO_TICKS(1000));
            unsigned elapsed=(unsigned)((esp_timer_get_time()-send_start)/1000);
            if(elapsed>atomic_load(&max_send_ms))atomic_store(&max_send_ms,elapsed);
            if(sent!=size)fail("Audio upload interrupted");
            else atomic_fetch_add(&input_frames_sent,1);
        }
    }
cleanup:
    atomic_store(&ready,false);audio_clear();
    if(client) {
        if(atomic_load(&connected)&&!atomic_load(&finalized)) {
            const char *close="{\"type\":\"session.close\"}";
            esp_websocket_client_send_text(client,close,strlen(close),pdMS_TO_TICKS(1000));
            for(int i=0;i<150&&!atomic_load(&finalized)&&atomic_load(&connected);i++)vTaskDelay(pdMS_TO_TICKS(100));
        }
        esp_websocket_client_stop(client);esp_websocket_client_destroy(client);
    }
    if(ws)esp_transport_destroy(ws);
    if(tls)esp_transport_destroy(tls);
    // Drain the response worker before permitting another session.
    char *barrier=NULL;
    xQueueSend(responses,&barrier,portMAX_DELAY);
    xSemaphoreTake(responses_idle,portMAX_DELAY);
    atomic_store(&ready,false);audio_clear();message_used=0;message_opcode=0;
    memset(headers,0,sizeof(headers));memset(key,0,sizeof(key));
    if(!atomic_load(&failed))state=atomic_load(&finalized)?"Ready; session closed":"Ready; final usage not confirmed";
    wake_word_resume_after_voice();atomic_store(&busy,false);vTaskDelete(NULL);
}
void live_voice_init(void) {
    static StaticQueue_t input_control;
    uint8_t *input_storage=heap_caps_malloc(32*sizeof(input_frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    configASSERT(input_storage);
    inputs=xQueueCreateStatic(32,sizeof(input_frame),input_storage,&input_control);message=malloc(MESSAGE_MAX);
    responses=xQueueCreate(16,sizeof(char *));responses_idle=xSemaphoreCreateBinary();
    configASSERT(inputs&&message&&responses&&responses_idle);
    configASSERT(xTaskCreate(response_task,"voice_responses",8192,NULL,4,NULL)==pdPASS);
    atomic_store(&usage,-1);
}
esp_err_t live_voice_start(void) {
    if(wake_word_testing())return ESP_ERR_INVALID_STATE;
    if(audio_usb_active())return ESP_ERR_INVALID_STATE;
    if(!credentials_present()){state="Enter your OpenAI API key in device setup";return ESP_ERR_INVALID_STATE;}
    bool expected=false;if(!atomic_compare_exchange_strong(&busy,&expected,true))return ESP_ERR_INVALID_STATE;
    atomic_store(&stopping,false);
    if(xTaskCreate(session_task,"live_voice",12288,NULL,4,NULL)!=pdPASS){atomic_store(&busy,false);return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
void live_voice_feed(const int16_t *samples,size_t count) {
    if(!atomic_load(&ready)||atomic_load(&stopping))return;
    while(count) {
        size_t take=count>512?512:count;input_frame frame={.bytes=take*2};memcpy(frame.samples,samples,take*2);
        if(xQueueSend(inputs,&frame,0)!=pdTRUE){fail("Microphone backlog; press to try again");return;}
        unsigned pending=uxQueueMessagesWaiting(inputs);
        if(pending>atomic_load(&input_queue_peak))atomic_store(&input_queue_peak,pending);
        samples+=take;count-=take;
    }
}
