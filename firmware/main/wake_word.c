#include "wake_word.h"
#include "activity_leds.h"
#include "live_voice.h"
#include "credentials.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

typedef struct {uint8_t version,enabled,phrase,sensitivity;} preferences;
typedef struct {size_t count;int16_t samples[256];} frame;
static preferences settings={1,1,0,50};
static SemaphoreHandle_t lock;
static QueueHandle_t audio;
static nvs_handle_t storage;
static atomic_bool available;
static atomic_bool release_requested, release_done;
bool wake_word_pause_for_voice(void){
    atomic_store(&release_done,false);atomic_store(&release_requested,true);
    for(int i=0;i<500 && !atomic_load(&release_done);i++)vTaskDelay(pdMS_TO_TICKS(10));
    return atomic_load(&release_done);
}
void wake_word_resume_after_voice(void){atomic_store(&release_requested,false);}
static atomic_uint detections,dropped;
static atomic_uint test_until;
static _Atomic(const char *) message="Loading wake word";
static uint32_t now_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
bool wake_word_testing(void){uint32_t until=atomic_load(&test_until);return until && (int32_t)(until-now_ms())>0;}
static preferences snapshot(void){if(!lock)return settings;xSemaphoreTake(lock,portMAX_DELAY);preferences p=settings;xSemaphoreGive(lock);return p;}
bool wake_word_listening(void){return atomic_load(&available) && !live_voice_active() && !audio_usb_active() && (snapshot().enabled || wake_word_testing());}
esp_err_t wake_word_configure(bool enabled,int phrase,int sensitivity){
    if(phrase<0||phrase>1||sensitivity<0||sensitivity>100)return ESP_ERR_INVALID_ARG;
    if(live_voice_active()||audio_usb_active())return ESP_ERR_INVALID_STATE;
    preferences next={1,enabled,phrase,sensitivity};
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t err=nvs_set_blob(storage,"preferences",&next,sizeof(next));
    if(err==ESP_OK)err=nvs_commit(storage);
    if(err==ESP_OK)settings=next;
    xSemaphoreGive(lock);return err;
}
esp_err_t wake_word_test(bool enabled){
    if(live_voice_active()||audio_usb_active())return ESP_ERR_INVALID_STATE;
    atomic_store(&test_until,enabled?now_ms()+60000:0);return ESP_OK;
}
cJSON *wake_word_status(void){
    preferences p=snapshot();cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"type","wake_status");
    cJSON_AddBoolToObject(j,"enabled",p.enabled);cJSON_AddNumberToObject(j,"phrase",p.phrase);
    cJSON_AddNumberToObject(j,"sensitivity",p.sensitivity);cJSON_AddBoolToObject(j,"ready",available);
    cJSON_AddBoolToObject(j,"testing",wake_word_testing());cJSON_AddNumberToObject(j,"detections",detections);
    cJSON_AddNumberToObject(j,"dropped",dropped);cJSON_AddStringToObject(j,"message",message);return j;
}
void wake_word_feed(const int16_t *samples,size_t count){
    if(!audio||!atomic_load(&available)||live_voice_active()||audio_usb_active())return;
    while(count){size_t n=count>256?256:count;frame f={.count=n};memcpy(f.samples,samples,n*2);
        if(xQueueSend(audio,&f,0)!=pdTRUE){atomic_fetch_add(&dropped,1);return;}
        samples+=n;count-=n;}
}
static void worker(void *unused){
    srmodel_list_t *models=esp_srmodel_init("model");
    if(!models){message="Wake models unavailable; BOOT still works";vTaskDelete(NULL);return;}
    const esp_wn_iface_t *engine=NULL;model_iface_data_t *model=NULL;
    int loaded=-1,sensitivity=-1,chunk=0;size_t used=0;int16_t *buffer=NULL;
    uint32_t cooldown=now_ms()+3000,seen_dropped=0;bool paused=true;
    for(;;){
        preferences p=snapshot();bool testing=wake_word_testing();
        if(atomic_load(&release_requested)||live_voice_active()||audio_usb_active()||(!p.enabled&&!testing)){
            message=live_voice_active()?"Conversation active":(!p.enabled?"Wake word disabled":"USB audio active");
            available=false;
            if(model){engine->destroy(model);model=NULL;}
            free(buffer);buffer=NULL;loaded=-1;
            if(atomic_load(&release_requested))atomic_store(&release_done,true);
            paused=true;used=0;xQueueReset(audio);
            cooldown=now_ms()+3000;vTaskDelay(pdMS_TO_TICKS(100));continue;
        }
        if(loaded!=p.phrase || sensitivity!=p.sensitivity){
            available=false;if(model)engine->destroy(model);model=NULL;free(buffer);buffer=NULL;
            const char *name=esp_srmodel_filter(models,"wn9",p.phrase?"computer":"jarvis");
            engine=name?esp_wn_handle_from_name(name):NULL;
            if(engine)model=engine->create(name,DET_MODE_90);
            if(!model){message="Wake model failed to load; BOOT still works";vTaskDelay(pdMS_TO_TICKS(1000));continue;}
            chunk=engine->get_samp_chunksize(model);
            if(chunk<=0||chunk>4096||engine->get_samp_rate(model)!=16000||engine->get_channel_num(model)!=1){
                engine->destroy(model);model=NULL;message="Unsupported wake model";vTaskDelay(pdMS_TO_TICKS(1000));continue;
            }
            buffer=heap_caps_aligned_alloc(16,chunk*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!buffer){engine->destroy(model);model=NULL;message="Wake memory unavailable";vTaskDelay(pdMS_TO_TICKS(1000));continue;}
            loaded=p.phrase;sensitivity=-1;used=0;xQueueReset(audio);available=true;
        }
        if(sensitivity!=p.sensitivity){
            // 50% preserves the manufacturer's threshold. Higher is more sensitive.
            engine->reset_det_threshold(model);float threshold=engine->get_det_threshold(model,1)+(50-p.sensitivity)*0.004f;
            if(threshold<0.4f)threshold=0.4f;
            if(threshold>0.95f)threshold=0.95f;
            engine->set_det_threshold(model,threshold,1);used=0;sensitivity=p.sensitivity;
        }
        paused=false;message=testing?"Detection test: say the wake word":"Listening locally for wake word";
        frame f;if(xQueueReceive(audio,&f,pdMS_TO_TICKS(100))!=pdTRUE)continue;
        if(seen_dropped!=atomic_load(&dropped)){seen_dropped=atomic_load(&dropped);loaded=-1;used=0;xQueueReset(audio);continue;}
        for(size_t offset=0;offset<f.count;){size_t n=chunk-used;if(n>f.count-offset)n=f.count-offset;
            memcpy(buffer+used,f.samples+offset,n*2);used+=n;offset+=n;
            if(used!=(size_t)chunk)continue;
            used=0;
            int detected=engine->detect(model,buffer);
            if(detected>0 && (int32_t)(now_ms()-cooldown)>=0){
                atomic_fetch_add(&detections,1);cooldown=now_ms()+3000;xQueueReset(audio);
                activity_leds_wake();
                // Re-check current mode at the action boundary, never use stale test state.
                preferences current=snapshot();
                if(wake_word_testing())audio_tone();
                else if(current.enabled && current.phrase==loaded){
                    // Startup owns Wi-Fi recovery and reports failures; never silently
                    // discard a valid wake just because the station lost its address.
                    if(live_voice_start()==ESP_OK)audio_tone();
                }
                loaded=-1;break;
            }
        }
    }
}
void wake_word_init(void){
    lock=xSemaphoreCreateMutex();configASSERT(lock);ESP_ERROR_CHECK(nvs_open("wake_word",NVS_READWRITE,&storage));
    preferences saved;size_t size=sizeof(saved);
    if(nvs_get_blob(storage,"preferences",&saved,&size)==ESP_OK && size==sizeof(saved) && saved.version==1 && saved.enabled<=1 && saved.phrase<=1 && saved.sensitivity<=100)settings=saved;
    static StaticQueue_t control;uint8_t *samples=heap_caps_malloc(16*sizeof(frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);configASSERT(samples);
    audio=xQueueCreateStatic(16,sizeof(frame),samples,&control);configASSERT(audio);
    configASSERT(xTaskCreatePinnedToCore(worker,"wake_word",12288,NULL,3,NULL,0)==pdPASS);
}
