#include "live_voice.h"
#include "credentials.h"
#include "setup_portal.h"
#include "wake_word.h"
#include "hangup_phrase.h"
#include "assistant.h"
#include "delegation.h"
#include "mic_control.h"
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
#include "esp_system.h"
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
enum { INPUT_QUEUE_FRAMES=64 };
typedef struct {uint8_t pcm[2048];char encoded[2733],json[2816];} upload_batch;
static upload_batch *upload;
static QueueHandle_t inputs;
static QueueHandle_t responses;
static QueueHandle_t tool_calls;
static atomic_uint searches,functions,backend_errors;
static atomic_uint pong_count;
static SemaphoreHandle_t responses_idle;
static atomic_bool busy, ready, stopping, connected, finalized, failed;
static atomic_uint activity_ms;
static atomic_int usage;
static atomic_uint input_frames_sent, max_send_ms, input_queue_peak;
static atomic_uint input_samples_dropped,upload_started_ms;
static atomic_uint response_queue_peak,response_queue_waits;
static atomic_int socket_error, tls_error;
static atomic_int close_code,transport_event,transport_error_type;
static atomic_uint session_started_ms;
static char transport_failure[200];
int live_voice_socket_error(void) { return atomic_load(&socket_error); }
int live_voice_tls_error(void) { return atomic_load(&tls_error); }
static _Atomic(const char *) state="Ready";
static char *message;
static size_t message_used;
static int message_opcode;
static hangup_phrase_t hangup_phrase;
static atomic_uint hangup_count;
typedef struct {unsigned count,uptime_ms,frames,max_send_ms,queue_peak,internal_free,internal_largest,response_peak,response_waits,session_ms;int close_code,event,error_type,socket_error,tls_error;char message[112];} failure_record;
static failure_record previous_failure;
static portMUX_TYPE failure_lock=portMUX_INITIALIZER_UNLOCKED;
cJSON *live_voice_upload_status(void){
    cJSON *j=cJSON_CreateObject();wifi_ap_record_t ap;
    cJSON_AddStringToObject(j,"type","upload_status");
    cJSON_AddNumberToObject(j,"reset_reason",esp_reset_reason());
    cJSON_AddNumberToObject(j,"uptime_ms",(unsigned)(esp_timer_get_time()/1000));
    cJSON_AddNumberToObject(j,"wifi_candidates",setup_portal_wifi_candidates());
    cJSON_AddNumberToObject(j,"wifi_selected_rssi",setup_portal_wifi_best_rssi());
    cJSON_AddNumberToObject(j,"dropped_ms",atomic_load(&input_samples_dropped)/16);
    unsigned began=atomic_load(&upload_started_ms);
    cJSON_AddNumberToObject(j,"inflight_ms",began?(unsigned)(esp_timer_get_time()/1000)-began:0);
    cJSON_AddNumberToObject(j,"queue_frames",uxQueueMessagesWaiting(inputs));
    cJSON_AddNumberToObject(j,"max_send_ms",atomic_load(&max_send_ms));
    cJSON_AddNumberToObject(j,"searches",atomic_load(&searches));
    cJSON_AddNumberToObject(j,"functions",atomic_load(&functions));
    cJSON_AddNumberToObject(j,"backend_errors",atomic_load(&backend_errors));
    cJSON_AddNumberToObject(j,"pongs",atomic_load(&pong_count));
    if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK)cJSON_AddNumberToObject(j,"wifi_rssi",ap.rssi);
    return j;
}
cJSON *live_voice_failure_status(void){
    portENTER_CRITICAL(&failure_lock);failure_record f=previous_failure;portEXIT_CRITICAL(&failure_lock);
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"type","voice_failure");
    cJSON_AddNumberToObject(j,"count",f.count);cJSON_AddStringToObject(j,"message",f.message);
    cJSON_AddNumberToObject(j,"uptime_ms",f.uptime_ms);cJSON_AddNumberToObject(j,"input_frames",f.frames);
    cJSON_AddNumberToObject(j,"max_send_ms",f.max_send_ms);cJSON_AddNumberToObject(j,"queue_peak",f.queue_peak);
    cJSON_AddNumberToObject(j,"internal_free",f.internal_free);cJSON_AddNumberToObject(j,"internal_largest",f.internal_largest);
    cJSON_AddNumberToObject(j,"response_peak",f.response_peak);cJSON_AddNumberToObject(j,"response_waits",f.response_waits);
    cJSON_AddNumberToObject(j,"session_ms",f.session_ms);cJSON_AddNumberToObject(j,"close_code",f.close_code);
    cJSON_AddNumberToObject(j,"event",f.event);cJSON_AddNumberToObject(j,"error_type",f.error_type);
    cJSON_AddNumberToObject(j,"socket_error",f.socket_error);cJSON_AddNumberToObject(j,"tls_error",f.tls_error);
    return j;
}
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
static void fail(const char *text) {
    bool expected=false;
    if(atomic_compare_exchange_strong(&failed,&expected,true)){
        failure_record f={.uptime_ms=(unsigned)(esp_timer_get_time()/1000),.frames=atomic_load(&input_frames_sent),
            .max_send_ms=atomic_load(&max_send_ms),.queue_peak=atomic_load(&input_queue_peak),
            .internal_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            .internal_largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)};
        f.response_peak=atomic_load(&response_queue_peak);f.response_waits=atomic_load(&response_queue_waits);
        f.session_ms=f.uptime_ms-atomic_load(&session_started_ms);
        f.close_code=atomic_load(&close_code);f.event=atomic_load(&transport_event);
        f.error_type=atomic_load(&transport_error_type);f.socket_error=atomic_load(&socket_error);f.tls_error=atomic_load(&tls_error);
        // Retain fixed application messages, never arbitrary provider payloads or headers.
        snprintf(f.message,sizeof(f.message),"%s",text==transport_failure?"Voice transport failed":text);
        portENTER_CRITICAL(&failure_lock);f.count=previous_failure.count+1;previous_failure=f;portEXIT_CRITICAL(&failure_lock);
        state=text;
    }
    live_voice_stop();
}

static void parse_message(const char *text) {
    cJSON *root=cJSON_Parse(text);
    if(!root) {fail("Invalid voice response");return;}
    cJSON *type=cJSON_GetObjectItem(root,"type");
    if(!cJSON_IsString(type)) {cJSON_Delete(root);return;}
    if(!strcmp(type->valuestring,"session.started")) {
        hangup_phrase_reset(&hangup_phrase);
        atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
        atomic_store(&ready,true);state="Listening";
        audio_tone();
    } else if(!strcmp(type->valuestring,"session.output_audio.delta") && !atomic_load(&stopping)) {
        cJSON *delta=cJSON_GetObjectItem(root,"delta");
        if(cJSON_IsString(delta)) {
            size_t encoded=strlen(delta->valuestring), capacity=encoded/4*3+4, decoded=0;
            uint8_t *pcm=heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!pcm || mbedtls_base64_decode(pcm,capacity,&decoded,(uint8_t *)delta->valuestring,encoded)!=0 || decoded%2)
                fail("Invalid voice audio");
            else if(decoded) {
                // Transcripts may arrive well ahead of speech. A long spoken answer
                // is activity even when no further transcript deltas are arriving.
                atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
                if(!audio_enqueue((int16_t *)pcm,decoded/2)) fail("Speaker backlog; press to try again");
            }
            free(pcm);
        }
    } else if(!strcmp(type->valuestring,"session.input_transcript.delta") || !strcmp(type->valuestring,"session.output_transcript.delta")) {
        cJSON *delta=cJSON_GetObjectItem(root,"delta");
        if(cJSON_IsString(delta)&&delta->valuestring[0]) atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
        if(!strcmp(type->valuestring,"session.input_transcript.delta") && cJSON_IsString(delta) && live_voice_ready())
            hangup_phrase_feed(&hangup_phrase,delta->valuestring,(uint32_t)(esp_timer_get_time()/1000));
    } else if(!strcmp(type->valuestring,"response.event") && !atomic_load(&stopping)) {
        cJSON *event=cJSON_GetObjectItem(root,"event"),*event_type=cJSON_GetObjectItem(event,"type");
        if(cJSON_IsString(event_type)) {
            atomic_store(&activity_ms,(uint32_t)(esp_timer_get_time()/1000));
            if(!strcmp(event_type->valuestring,"response.web_search_call.completed"))atomic_fetch_add(&searches,1);
            if(!strcmp(event_type->valuestring,"response.failed"))atomic_fetch_add(&backend_errors,1);
        }
        cJSON *calls=delegation_event(root);
        if(calls && xQueueSend(tool_calls,&calls,0)!=pdTRUE){cJSON_Delete(calls);fail("Device tool queue full; try again");}
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
    if(id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED){
        transport_event=id;transport_error_type=event->error_handle.error_type;
        if(event->close_status_code)close_code=event->close_status_code;
        // The SDK initializes these fields only for TCP transport errors.
        if(event->error_handle.error_type==WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT){
            socket_error=event->error_handle.esp_transport_sock_errno;
            tls_error=event->error_handle.esp_tls_stack_err;
        }
        // The SDK can report the close code after its initial error callback.
        // Enrich the same failure without changing its original message/count.
        if(atomic_load(&failed)){
            portENTER_CRITICAL(&failure_lock);
            previous_failure.close_code=atomic_load(&close_code);previous_failure.event=id;
            previous_failure.error_type=atomic_load(&transport_error_type);
            previous_failure.socket_error=atomic_load(&socket_error);previous_failure.tls_error=atomic_load(&tls_error);
            portEXIT_CRITICAL(&failure_lock);
        }
    }
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
    if(id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED) {
        if(!atomic_load(&stopping)) fail(event->error_handle.error_type==WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT?
            "Voice heartbeat timed out; press to reconnect":"Voice connection ended; press to reconnect");
        atomic_store(&connected,false);return;
    }
    if(id==WEBSOCKET_EVENT_DATA && event->op_code==WS_TRANSPORT_OPCODES_PONG)atomic_fetch_add(&pong_count,1);
    if(id!=WEBSOCKET_EVENT_DATA || (event->op_code!=1 && event->op_code!=0)) return;
    if(event->payload_offset==0 && event->op_code==1) {message_used=0;message_opcode=1;}
    if(message_opcode!=1 || event->data_len<0 || message_used+event->data_len>=MESSAGE_MAX) {fail("Voice response exceeds buffer");return;}
    memcpy(message+message_used,event->data_ptr,event->data_len);message_used+=event->data_len;
    if(event->fin && event->payload_offset+event->data_len==event->payload_len) {
        message[message_used]=0;
        char *copy=heap_caps_malloc(message_used+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!copy) {
            fail("Voice response allocation failed");
        } else {
            memcpy(copy,message,message_used+1);
            BaseType_t queued=xQueueSend(responses,&copy,0);
            if(queued!=pdTRUE){
                // The websocket task can receive a burst faster than the lower-priority
                // decoder runs. Yield with bounded backpressure instead of dropping a
                // response immediately. TX uses its separate lock, so mic upload continues.
                atomic_fetch_add(&response_queue_waits,1);
                queued=xQueueSend(responses,&copy,pdMS_TO_TICKS(250));
            }
            if(queued!=pdTRUE){
                free(copy);
                if(!atomic_load(&stopping))fail("Voice response backlog; press to try again");
            } else {
                unsigned depth=uxQueueMessagesWaiting(responses);
                if(depth>atomic_load(&response_queue_peak))atomic_store(&response_queue_peak,depth);
            }
        }
        message_used=0;message_opcode=0;
    }
}
static bool send_json(esp_websocket_client_handle_t client,cJSON *event) {
    char *text=cJSON_PrintUnformatted(event);cJSON_Delete(event);
    if(!text)return false;
    size_t n=strlen(text);
    bool ok=esp_websocket_client_send_text(client,text,n,pdMS_TO_TICKS(1000))==(int)n;
    free(text);return ok;
}
static bool run_tools(esp_websocket_client_handle_t client,cJSON *calls) {
    if(cJSON_IsObject(calls)) {
        cJSON *generation=cJSON_GetObjectItem(calls,"generation");
        if(!generation || generation->valuedouble!=atomic_load(&session_started_ms))return true;
        cJSON *event=cJSON_DetachItemFromObject(calls,"request");
        bool instruction=cJSON_IsTrue(cJSON_GetObjectItem(calls,"instruction"));
        return send_json(client,event) && (instruction || send_json(client,cJSON_Parse("{\"type\":\"response.create\"}")));
    }
    cJSON *call;
    cJSON_ArrayForEach(call,calls) {
        if(atomic_load(&stopping))return false;
        cJSON *result=assistant_execute(cJSON_GetObjectItem(call,"name")->valuestring,cJSON_GetObjectItem(call,"arguments")->valuestring);
        char *output=cJSON_PrintUnformatted(result);cJSON_Delete(result);
        if(!output)return false;
        cJSON *event=cJSON_CreateObject(),*item=cJSON_AddObjectToObject(event,"item");
        cJSON_AddStringToObject(event,"type","response.item.create");
        cJSON_AddStringToObject(item,"type","function_call_output");
        cJSON_AddStringToObject(item,"call_id",cJSON_GetObjectItem(call,"call_id")->valuestring);
        cJSON_AddStringToObject(item,"output",output);free(output);
        if(!send_json(client,event))return false;
        atomic_fetch_add(&functions,1);
    }
    return send_json(client,cJSON_Parse("{\"type\":\"response.create\"}"));
}
esp_err_t live_voice_text(const char *text) {
    if(!live_voice_ready() || !text || !text[0] || strlen(text)>600)return ESP_ERR_INVALID_STATE;
    cJSON *j=cJSON_Parse("{\"request\":{\"type\":\"response.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\",\"content\":[]}}}");
    if(!j)return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(j,"generation",atomic_load(&session_started_ms));
    cJSON *content=cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j,"request"),"item"),"content");
    cJSON *part=cJSON_CreateObject();cJSON_AddStringToObject(part,"type","input_text");cJSON_AddStringToObject(part,"text",text);cJSON_AddItemToArray(content,part);
    if(xQueueSend(tool_calls,&j,0)!=pdTRUE){cJSON_Delete(j);return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
bool live_voice_instruction(const char *text) {
    if(!live_voice_ready())return false;
    cJSON *j=cJSON_CreateObject(),*event=cJSON_AddObjectToObject(j,"request");
    cJSON_AddNumberToObject(j,"generation",atomic_load(&session_started_ms));cJSON_AddBoolToObject(j,"instruction",true);
    cJSON_AddStringToObject(event,"type","session.instructions.append");cJSON_AddNullToObject(event,"delegation_id");cJSON_AddStringToObject(event,"content",text);
    if(!j||!event||xQueueSend(tool_calls,&j,0)!=pdTRUE){cJSON_Delete(j);return false;}
    return true;
}

static void session_task(void *unused) {
    esp_websocket_client_handle_t client=NULL;
    esp_transport_handle_t tls=NULL, ws=NULL;
    const char *limit_reason=NULL;
    char *start_event=NULL;
    char key[513]={0},headers[560]={0};
    atomic_store(&ready,false);atomic_store(&connected,false);atomic_store(&finalized,false);atomic_store(&failed,false);atomic_store(&usage,-1);
    xQueueReset(inputs);audio_clear();state="Connecting Wi-Fi";
    input_frames_sent=0;max_send_ms=0;input_queue_peak=0;
    input_samples_dropped=0;upload_started_ms=0;
    response_queue_peak=0;response_queue_waits=0;
    searches=0;functions=0;backend_errors=0;pong_count=0;delegation_reset();
    socket_error=0;tls_error=0;close_code=0;transport_event=0;transport_error_type=0;
    session_started_ms=(unsigned)(esp_timer_get_time()/1000);
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
    // External transports bypass the client's normal transport configuration.
    // Forward PONG/CLOSE frames so its heartbeat and close handling can see them.
    const esp_transport_ws_config_t ws_config={.ws_path="/v1/live/sessions",.headers=headers,.propagate_control_frames=true};
    if(esp_transport_ws_set_config(ws,&ws_config)!=ESP_OK){fail("Could not configure voice transport");goto cleanup;}
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
    start_event=assistant_start_event();
    if(!start_event || esp_websocket_client_send_text(client,start_event,strlen(start_event),pdMS_TO_TICKS(2000))!=(int)strlen(start_event)){fail("Session startup failed");goto cleanup;}
    free(start_event);start_event=NULL;
    uint32_t began=esp_timer_get_time()/1000;
    while(!atomic_load(&stopping)) {
        mic_control_tick();
        uint32_t now=esp_timer_get_time()/1000;
        if(!atomic_load(&ready)&&now-began>20000){fail("Voice startup timed out");break;}
        if(now-began>600000)limit_reason="Ready; 10-minute conversation limit reached";
        // The response worker may update activity after this loop sampled now.
        // Signed elapsed time treats that newer timestamp as active, not a huge
        // unsigned underflow. Both deadlines are far below the half-wrap interval.
        else if(atomic_load(&ready)&&(int32_t)(now-atomic_load(&activity_ms))>60000)limit_reason="Ready; 60-second idle timeout";
        if(limit_reason){state=limit_reason;live_voice_stop();break;}
        cJSON *calls=NULL;
        if(xQueueReceive(tool_calls,&calls,0)==pdTRUE) {
            bool ok=run_tools(client,calls);cJSON_Delete(calls);
            if(!ok){if(!atomic_load(&stopping))fail("Device tool response failed");break;}
        }
        input_frame frame;
        if(xQueueReceive(inputs,&frame,pdMS_TO_TICKS(20))==pdTRUE && atomic_load(&ready)) {
            size_t bytes=frame.bytes,length=0;unsigned frames=1;
            memcpy(upload->pcm,frame.samples,bytes);
            // Coalesce up to 64 ms of PCM rather than doing a TLS/WebSocket send
            // for each 16 ms AEC chunk. Peek preserves a frame that will not fit.
            while(bytes<sizeof(upload->pcm) && frames<4 && !atomic_load(&stopping)) {
                if(xQueuePeek(inputs,&frame,pdMS_TO_TICKS(20))!=pdTRUE)break;
                if(frame.bytes>sizeof(upload->pcm)-bytes)break;
                if(xQueueReceive(inputs,&frame,0)!=pdTRUE)break;
                memcpy(upload->pcm+bytes,frame.samples,frame.bytes);bytes+=frame.bytes;frames++;
            }
            if(atomic_load(&stopping))break;
            if(mbedtls_base64_encode((uint8_t *)upload->encoded,sizeof(upload->encoded),&length,upload->pcm,bytes)!=0){fail("Audio encoding failed");break;}
            upload->encoded[length]=0;
            int size=snprintf(upload->json,sizeof(upload->json),"{\"type\":\"session.input_audio.append\",\"audio\":\"%s\"}",upload->encoded);
            if(size<0 || size>=sizeof(upload->json)){fail("Audio encoding failed");break;}
            int64_t send_start=esp_timer_get_time();
            atomic_store(&upload_started_ms,(unsigned)(send_start/1000));
            int sent=esp_websocket_client_send_text(client,upload->json,size,pdMS_TO_TICKS(1000));
            atomic_store(&upload_started_ms,0);
            unsigned elapsed=(unsigned)((esp_timer_get_time()-send_start)/1000);
            if(elapsed>atomic_load(&max_send_ms))atomic_store(&max_send_ms,elapsed);
            if(sent!=size)fail("Audio upload interrupted");
            else atomic_fetch_add(&input_frames_sent,frames);
        }
    }
cleanup:
    mic_control_cancel();
    free(start_event);
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
    cJSON *unused_calls=NULL;
    while(xQueueReceive(tool_calls,&unused_calls,0)==pdTRUE)cJSON_Delete(unused_calls);
    delegation_reset();
    atomic_store(&ready,false);audio_clear();message_used=0;message_opcode=0;
    memset(headers,0,sizeof(headers));memset(key,0,sizeof(key));
    if(!atomic_load(&failed))state=limit_reason?limit_reason:(atomic_load(&finalized)?"Ready; session closed":"Ready; final usage not confirmed");
    wake_word_resume_after_voice();atomic_store(&busy,false);vTaskDelete(NULL);
}
void live_voice_init(void) {
    static StaticQueue_t input_control;
    uint8_t *input_storage=heap_caps_malloc(INPUT_QUEUE_FRAMES*sizeof(input_frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    upload=heap_caps_malloc(sizeof(upload_batch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    configASSERT(input_storage && upload);
    inputs=xQueueCreateStatic(INPUT_QUEUE_FRAMES,sizeof(input_frame),input_storage,&input_control);message=malloc(MESSAGE_MAX);
    responses=xQueueCreate(16,sizeof(char *));responses_idle=xSemaphoreCreateBinary();
    tool_calls=xQueueCreate(4,sizeof(cJSON *));
    configASSERT(inputs&&message&&responses&&responses_idle&&tool_calls);
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
        if(xQueueSend(inputs,&frame,0)!=pdTRUE){
            // Keep capture nonblocking and prefer current speech to stale audio.
            // The upload task still reports actual transport/send failures.
            input_frame stale;
            if(xQueueReceive(inputs,&stale,0)==pdTRUE)atomic_fetch_add(&input_samples_dropped,stale.bytes/2);
            if(xQueueSend(inputs,&frame,0)!=pdTRUE)atomic_fetch_add(&input_samples_dropped,take);
        }
        unsigned pending=uxQueueMessagesWaiting(inputs);
        if(pending>atomic_load(&input_queue_peak))atomic_store(&input_queue_peak,pending);
        samples+=take;count-=take;
    }
}
