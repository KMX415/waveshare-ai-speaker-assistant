#include "firmware_update.h"
#include "update_public_key.h"
#include "update_test_fixture.h"
#include "esp_sntp.h"
#include <time.h>
#include "runtime_gate.h"
#include "live_voice.h"
#include "mic_control.h"
#include "wake_word.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MANIFEST_URL "https://github.com/KMX415/waveshare-ai-speaker-assistant/releases/latest/download/update.json"
#define PREVIEW_MANIFEST_URL "https://github.com/KMX415/waveshare-ai-speaker-assistant/releases/download/ota-preview/update.json"
typedef struct { char url[385], version[32]; uint8_t digest[32]; unsigned size,release; } candidate;
static SemaphoreHandle_t mutex;
static atomic_int requested; // 0 none, 1 check, 2 install
static atomic_bool running;
static atomic_bool service_ready;
static atomic_int last_http_status,last_http_error,last_redirects;
static char state[96]="Not checked", available_version[32];
static unsigned available_release;

static void message(const char *text){xSemaphoreTake(mutex,portMAX_DELAY);snprintf(state,sizeof(state),"%s",text);xSemaphoreGive(mutex);}
static bool ota_layout(void) {
    const esp_partition_t *next=esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *current=esp_ota_get_running_partition();
    return next && current && next->address!=current->address &&
        current->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_0 && current->subtype<=ESP_PARTITION_SUBTYPE_APP_OTA_MAX &&
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,NULL)!=NULL;
}
static bool integer(const cJSON *v,unsigned low,unsigned high) {
    return cJSON_IsNumber(v)&&v->valuedouble>=low&&v->valuedouble<=high&&v->valuedouble==v->valueint;
}
static bool equals(const cJSON *j,const char *key,const char *value){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)&&!strcmp(v->valuestring,value);}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static bool unique(const cJSON *j){const cJSON *v;cJSON_ArrayForEach(v,j)if(!v->string||cJSON_GetObjectItemCaseSensitive(j,v->string)!=v)return false;return true;}
static bool verify(const char *text,candidate *out) {
    bool ok=false;size_t length=0,sig_length=0;
    uint8_t raw[1025]={0},sig[80],hash[32];
    cJSON *envelope=cJSON_Parse(text),*payload=NULL;
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    if(!cJSON_IsObject(envelope)||cJSON_GetArraySize(envelope)!=2||!unique(envelope))goto done;
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(envelope,"payload"),*s=cJSON_GetObjectItemCaseSensitive(envelope,"signature");
    if(!cJSON_IsString(p)||!cJSON_IsString(s))goto done;
    if(mbedtls_base64_decode(raw,1024,&length,(unsigned char*)p->valuestring,strlen(p->valuestring))||!length||memchr(raw,0,length))goto done;
    if(mbedtls_base64_decode(sig,sizeof(sig),&sig_length,(unsigned char*)s->valuestring,strlen(s->valuestring))||!sig_length)goto done;
    if(mbedtls_sha256(raw,length,hash,0)||mbedtls_pk_parse_public_key(&key,(const unsigned char*)update_public_key,sizeof(update_public_key)))goto done;
    if(mbedtls_pk_verify(&key,MBEDTLS_MD_SHA256,hash,sizeof(hash),sig,sig_length))goto done;
    payload=cJSON_ParseWithOpts((char*)raw,NULL,true);
    if(!cJSON_IsObject(payload)||cJSON_GetArraySize(payload)!=8||!unique(payload))goto done;
    if(!equals(payload,"board",HOME_VOICE_BOARD)||!equals(payload,"assets",HOME_VOICE_ASSETS))goto done;
    const cJSON *layout=cJSON_GetObjectItemCaseSensitive(payload,"layout"),*release=cJSON_GetObjectItemCaseSensitive(payload,"release"),*size=cJSON_GetObjectItemCaseSensitive(payload,"size");
    const cJSON *url=cJSON_GetObjectItemCaseSensitive(payload,"url"),*version=cJSON_GetObjectItemCaseSensitive(payload,"version"),*digest=cJSON_GetObjectItemCaseSensitive(payload,"sha256");
    if(!integer(layout,1,1)||!integer(release,1,2147483647)||!integer(size,32,0x3f0000))goto done;
    if(!cJSON_IsString(url)||strlen(url->valuestring)>384||strncmp(url->valuestring,"https://",8)||strlen(url->valuestring)<10||strchr(url->valuestring,'@')||strchr(url->valuestring,'#'))goto done;
    for(const char *c=url->valuestring;*c;c++)if((unsigned char)*c<=32||(unsigned char)*c>=127)goto done;
    if(!cJSON_IsString(version)||!version->valuestring[0]||strlen(version->valuestring)>31)goto done;
    for(const char *c=version->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))goto done;
    if(!cJSON_IsString(digest)||strlen(digest->valuestring)!=64)goto done;
    for(int i=0;i<32;i++){int a=hex(digest->valuestring[i*2]),b=hex(digest->valuestring[i*2+1]);if(a<0||b<0)goto done;out->digest[i]=(a<<4)|b;}
    strcpy(out->url,url->valuestring);strcpy(out->version,version->valuestring);out->size=size->valueint;out->release=release->valueint;ok=true;
done:
    mbedtls_pk_free(&key);cJSON_Delete(payload);cJSON_Delete(envelope);return ok;
}
// Explicit redirect handling keeps HTTPS mandatory even after GitHub redirects.
static esp_http_client_handle_t open_https(const char *url) {
    last_http_status=0;last_http_error=0;last_redirects=0;
    // GitHub asset redirects contain long signed query strings; the default
    // 512-byte transmit buffer cannot hold their HTTP request line.
    esp_http_client_config_t cfg={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=15000,.buffer_size=2048,.buffer_size_tx=4096,.disable_auto_redirect=true};
    esp_http_client_handle_t h=esp_http_client_init(&cfg);if(!h)return NULL;
    for(int i=0;i<5;i++) {
        if(esp_http_client_get_transport_type(h)!=HTTP_TRANSPORT_OVER_SSL){last_http_error=ESP_ERR_INVALID_ARG;break;}
        esp_err_t opened=esp_http_client_open(h,0);
        if(opened!=ESP_OK){last_http_error=opened;break;}
        int64_t length=esp_http_client_fetch_headers(h);
        if(length<0){last_http_error=(int)length;break;}
        int code=esp_http_client_get_status_code(h);
        last_http_status=code;
        if(code==200)return h;
        esp_http_client_close(h);
        if(code!=301&&code!=302&&code!=303&&code!=307&&code!=308)break;
        esp_err_t redirected=esp_http_client_set_redirection(h);
        if(redirected!=ESP_OK){last_http_error=redirected;break;}
        last_redirects=i+1;
    }
    esp_http_client_cleanup(h);return NULL;
}
static bool fetch_manifest(candidate *out,bool preview) {
    esp_netif_t *station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip={0};
    for(int i=0;i<100;i++) {
        if(station&&esp_netif_get_ip_info(station,&ip)==ESP_OK&&ip.ip.addr)break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(!ip.ip.addr){message("Wi-Fi is not connected; retry later");return false;}
    if(!esp_sntp_enabled()){esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();}
    for(int i=0;i<100&&time(NULL)<1700000000;i++)vTaskDelay(pdMS_TO_TICKS(100));
    if(time(NULL)<1700000000){message("Clock synchronization unavailable; retry later");return false;}
    esp_http_client_handle_t h=open_https(preview?PREVIEW_MANIFEST_URL:MANIFEST_URL);if(!h){message("Release manifest unavailable");return false;}
    char *buffer=calloc(1,2049);size_t used=0;bool ok=false;
    if(buffer){
        int n=0;
        while(used<2048&&(n=esp_http_client_read(h,buffer+used,2048-used))>0)used+=n;
        if(n==0&&used<2048&&esp_http_client_is_complete_data_received(h)&&!memchr(buffer,0,used))ok=verify(buffer,out);
        free(buffer);
    }
    esp_http_client_close(h);esp_http_client_cleanup(h);
    if(!ok)message("Release manifest failed signature or compatibility checks");
    return ok;
}
static bool install(const candidate *c) {
    const esp_partition_t *next=esp_ota_get_next_update_partition(NULL);
    if(!next||c->size>next->size){message("Update does not fit the inactive slot");return false;}
    esp_http_client_handle_t h=open_https(c->url);
    if(!h){message("Firmware download unavailable");return false;}
    esp_ota_handle_t ota=0;bool began=false,ok=false;unsigned used=0;
    uint8_t *buffer=malloc(4096),digest[32];mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    if(!buffer||mbedtls_sha256_starts(&sha,0))goto done;
    if(esp_ota_begin(next,c->size,&ota)!=ESP_OK)goto done;
    began=true;
    int64_t deadline=esp_timer_get_time()+180000000;
    while(used<c->size&&esp_timer_get_time()<deadline) {
        unsigned want=c->size-used;if(want>4096)want=4096;
        int n=esp_http_client_read(h,(char*)buffer,want);
        if(n<=0||(!used&&buffer[0]!=0xe9))goto done;
        if(mbedtls_sha256_update(&sha,buffer,n)||esp_ota_write(ota,buffer,n)!=ESP_OK)goto done;
        used+=n;
    }
    if(used!=c->size||esp_http_client_read(h,(char*)buffer,1)!=0||!esp_http_client_is_complete_data_received(h))goto done;
    if(mbedtls_sha256_finish(&sha,digest)||memcmp(digest,c->digest,32))goto done;
    // Check descriptor before boot selection, in addition to the signed manifest.
    esp_app_desc_t desc;
    if(esp_ota_get_partition_description(next,&desc)!=ESP_OK||strcmp(desc.project_name,"home_voice_satellite")||strcmp(desc.version,c->version))goto done;
    esp_err_t err=esp_ota_end(ota);began=false;
    if(err!=ESP_OK||esp_ota_set_boot_partition(next)!=ESP_OK)goto done;
    ok=true;
done:
    if(began)esp_ota_abort(ota);
    free(buffer);mbedtls_sha256_free(&sha);esp_http_client_close(h);esp_http_client_cleanup(h);
    if(!ok)message("Update failed; existing firmware retained");
    return ok;
}
static void worker(void *unused) {
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if(!atomic_load(&requested)||live_voice_active()||mic_control_active()||audio_usb_active()||audio_is_playing()||wake_word_testing())continue;
        if(!runtime_acquire(RUNTIME_UPDATE))continue;
        if(audio_usb_active()||audio_is_playing()||mic_control_active()||wake_word_testing()){runtime_release(RUNTIME_UPDATE);continue;}
        int action=atomic_exchange(&requested,0);running=true;
        if(wake_word_pause_for_voice()) {
            candidate c={0};message("Checking signed release");
            if(fetch_manifest(&c,(action&4)!=0)) {
                xSemaphoreTake(mutex,portMAX_DELAY);strcpy(available_version,c.version);available_release=c.release;xSemaphoreGive(mutex);
                if(c.release<=HOME_VOICE_RELEASE)message("Already up to date");
                else if((action&3)==1)message("Update available");
                else {message("Installing into inactive slot");if(install(&c)){message("Restarting into new firmware");vTaskDelay(pdMS_TO_TICKS(500));esp_restart();}}
            }
        } else message("Audio resources busy; retry later");
        wake_word_resume_after_voice();running=false;runtime_release(RUNTIME_UPDATE);
    }
}
static void validate_boot(void *unused) {
    // No cloud dependency: local startup and wake-model availability are the gate.
    vTaskDelay(pdMS_TO_TICKS(15000));
    esp_ota_img_states_t boot_state;
    const esp_partition_t *partition=esp_ota_get_running_partition();
    if(partition&&esp_ota_get_state_partition(partition,&boot_state)==ESP_OK&&boot_state==ESP_OTA_IMG_PENDING_VERIFY) {
        cJSON *wake=wake_word_status();
        const cJSON *enabled=cJSON_GetObjectItem(wake,"enabled");
        bool healthy=audio_healthy()&&mic_control_selftest()==255&&(!cJSON_IsTrue(enabled)||wake_word_listening());
        cJSON_Delete(wake);
        if(healthy)esp_ota_mark_app_valid_cancel_rollback();
        else esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    runtime_release(RUNTIME_UPDATE);
    vTaskDelete(NULL);
}
void firmware_update_init(void) {
    mutex=xSemaphoreCreateMutex();configASSERT(mutex);
    if(!ota_layout()){message("USB migration required for Wi-Fi updates");return;}
    service_ready=xTaskCreate(worker,"firmware_update",8192,NULL,2,NULL)==pdPASS;
    if(!service_ready)message("Updater unavailable: insufficient memory");
    esp_ota_img_states_t boot_state;
    const esp_partition_t *partition=esp_ota_get_running_partition();
    if(partition&&esp_ota_get_state_partition(partition,&boot_state)==ESP_OK&&boot_state==ESP_OTA_IMG_PENDING_VERIFY) {
        configASSERT(runtime_acquire(RUNTIME_UPDATE));
        configASSERT(xTaskCreate(validate_boot,"update_boot_check",4096,NULL,2,NULL)==pdPASS);
    }
}
static esp_err_t request(int action) {
    if(!ota_layout()||!service_ready)return ESP_ERR_NOT_SUPPORTED;
    if(atomic_load(&running))return ESP_ERR_INVALID_STATE;
    int empty=0;if(!atomic_compare_exchange_strong(&requested,&empty,action))return ESP_ERR_INVALID_STATE;
    message("Queued until conversation and playback finish");return ESP_OK;
}
esp_err_t firmware_update_request(bool install_update){return request(install_update?2:1);}
esp_err_t firmware_update_preview(bool install_update){return request(install_update?6:5);}
cJSON *firmware_update_status(void) {
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"type","update_status");
    cJSON_AddStringToObject(j,"version",HOME_VOICE_VERSION);cJSON_AddNumberToObject(j,"release",HOME_VOICE_RELEASE);
    cJSON_AddNumberToObject(j,"http_status",last_http_status);cJSON_AddNumberToObject(j,"http_error",last_http_error);cJSON_AddNumberToObject(j,"redirects",last_redirects);
    cJSON_AddBoolToObject(j,"supported",ota_layout());cJSON_AddBoolToObject(j,"busy",atomic_load(&running)||runtime_updating());cJSON_AddBoolToObject(j,"queued",atomic_load(&requested)!=0);
    if(mutex){xSemaphoreTake(mutex,portMAX_DELAY);cJSON_AddStringToObject(j,"state",state);cJSON_AddStringToObject(j,"available_version",available_version);cJSON_AddNumberToObject(j,"available_release",available_release);xSemaphoreGive(mutex);}return j;
}

unsigned firmware_update_selftest(void) {
    candidate c={0};unsigned mask=0;
    if(verify(update_test_fixture,&c))mask|=1;
    if(c.release<HOME_VOICE_RELEASE)mask|=2;
    if(!verify("{}",&c))mask|=4;
    cJSON *j=cJSON_Parse(update_test_fixture);
    cJSON *sig=cJSON_GetObjectItem(j,"signature");
    if(cJSON_IsString(sig))sig->valuestring[0]=sig->valuestring[0]=='A'?'B':'A';
    char *text=cJSON_PrintUnformatted(j);
    if(text&&!verify(text,&c))mask|=8;
    free(text);cJSON_Delete(j);
    return mask;
}
