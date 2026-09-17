#include "setup_portal.h"
#include "board_audio.h"
#include "credentials.h"
#include "live_voice.h"
#include "wake_word.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include <unistd.h>

static nvs_handle_t storage;
static esp_netif_t *station;
static char ap_name[32], ap_password[25], saved_ssid[33], postal[17];
static int32_t volume = 40, gain = 24;
static bool ap_enabled;
static esp_timer_handle_t reconnect_timer;
static int startup_candidates, startup_best_rssi=-127;
int setup_portal_wifi_candidates(void){return startup_candidates;}
int setup_portal_wifi_best_rssi(void){return startup_best_rssi;}
static void select_startup_access_point(wifi_config_t *config){
    char ssid[33]={0};memcpy(ssid,config->sta.ssid,32);
    wifi_scan_config_t scan={.ssid=(uint8_t *)ssid,.show_hidden=true};
    if(esp_wifi_scan_start(&scan,true)!=ESP_OK)return;
    uint16_t count=32;
    wifi_ap_record_t *records=calloc(count,sizeof(*records));
    if(!records){esp_wifi_clear_ap_list();return;}
    if(esp_wifi_scan_get_ap_records(&count,records)==ESP_OK){
        int best=-1;
        for(unsigned i=0;i<count;i++){
            if(strncmp((char *)records[i].ssid,ssid,32))continue;
            startup_candidates++;
            if(best<0 || records[i].rssi>records[best].rssi)best=i;
        }
        if(best>=0){
            startup_best_rssi=records[best].rssi;
            memcpy(config->sta.bssid,records[best].bssid,6);
            config->sta.bssid_set=true;config->sta.channel=records[best].primary;
        }
    }
    free(records);
}
static void reconnect_wifi(void *unused) {
    wifi_config_t config={0};
    if(esp_wifi_get_config(WIFI_IF_STA,&config)==ESP_OK && config.sta.ssid[0])esp_wifi_connect();
    memset(&config,0,sizeof(config));
}
static void wifi_event(void *unused,esp_event_base_t base,int32_t id,void *data) {
    if(id==WIFI_EVENT_STA_DISCONNECTED) {
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer,5000000);
    } else if(id==WIFI_EVENT_STA_CONNECTED)esp_timer_stop(reconnect_timer);
}
int setup_portal_volume(void) { return volume; }
esp_err_t setup_portal_set_volume(int value) {
    if (value < 0 || value > 80) return ESP_ERR_INVALID_ARG;
    esp_err_t result = board_audio_volume(value);
    if (result == ESP_OK) result = nvs_set_i32(storage, "volume", value);
    if (result == ESP_OK) result = nvs_commit(storage);
    if (result == ESP_OK) volume = value;
    return result;
}
extern const char html_start[] asm("_binary_setup_html_start");
extern const char html_end[] asm("_binary_setup_html_end");

const char *setup_portal_password(void) { return ap_password; }
const char *setup_portal_ssid(void) { return ap_name; }
static void load_string(const char *key, char *out, size_t size) {
    if (nvs_get_str(storage, key, out, &size) != ESP_OK) out[0] = 0;
}
static esp_err_t json_response(httpd_req_t *r, cJSON *value) {
    char *text = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(r, text);
    free(text);
    return result;
}
static esp_err_t error(httpd_req_t *r, const char *message) {
    httpd_resp_set_status(r, "400 Bad Request");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", message);
    return json_response(r, body);
}
static bool permitted(httpd_req_t *r) {
    // Settings are exposed only on the password-protected setup AP, never the home LAN.
    // ESP-IDF's HTTP listener is dual-stack. IPv4 clients can arrive as
    // IPv4-mapped IPv6 addresses; a sockaddr_in buffer truncates that result.
    struct sockaddr_storage address = {0};
    socklen_t length = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&address, &length) != 0) return false;
    uint32_t local_ip = 0;
    if (address.ss_family == AF_INET) {
        local_ip = ((struct sockaddr_in *)&address)->sin_addr.s_addr;
    }
#if LWIP_IPV6
    else if (address.ss_family == AF_INET6) {
        const uint8_t *bytes = (const uint8_t *)&((struct sockaddr_in6 *)&address)->sin6_addr;
        static const uint8_t mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        if (memcmp(bytes, mapped_prefix, sizeof(mapped_prefix)) != 0) return false;
        memcpy(&local_ip, bytes + 12, sizeof(local_ip));
    }
#endif
    else return false;
    if (local_ip != inet_addr("192.168.4.1")) return false;
    char host[64];
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host,"192.168.4.1") && strcmp(host,"192.168.4.1:80"))) return false;
    if (r->method == HTTP_POST) {
        char guard[8], origin[64];
        if (httpd_req_get_hdr_value_str(r,"X-Home-Voice",guard,sizeof(guard)) != ESP_OK || strcmp(guard,"1")) return false;
        if (httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof(origin)) == ESP_OK && strcmp(origin,"http://192.168.4.1")) return false;
    }
    return true;
}
// Exercise the real dual-stack HTTP listener without reading or changing credentials.
static bool check_http(const char *request, int expected) {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return false;
    struct timeval timeout={.tv_sec=2};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(80)};
    target.sin_addr.s_addr=inet_addr("192.168.4.1");
    bool ok=false;
    if(connect(fd,(struct sockaddr *)&target,sizeof(target))==0) {
        size_t length=strlen(request),sent=0;
        while(sent<length) {int n=send(fd,request+sent,length-sent,0);if(n<=0)break;sent+=n;}
        char response[128]={0};size_t used=0;
        while(sent==length && used<sizeof(response)-1) {
            int n=recv(fd,response+used,sizeof(response)-1-used,0);if(n<=0)break;
            used+=n;if(strstr(response,"\r\n"))break;
        }
        int status=0;
        if(sscanf(response,"HTTP/1.1 %d",&status)==1)ok=status==expected;
    }
    close(fd);return ok;
}
uint32_t setup_portal_selftest(void) {
    if(live_voice_active())return 0;
    setup_portal_enable();
    uint32_t result=0;
    if(check_http("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n",200))result|=1;
    if(check_http("GET /api/status HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n",200))result|=2;
    if(check_http("GET / HTTP/1.1\r\nHost: untrusted.invalid\r\nConnection: close\r\n\r\n",403))result|=4;
    if(check_http("POST /api/preferences HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",403))result|=8;
    return result;
}
static esp_err_t handle(httpd_req_t *r) {
    if (!permitted(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Use the device setup Wi-Fi network");
    if (!strcmp(r->uri,"/")) {
        httpd_resp_set_type(r,"text/html");
        httpd_resp_set_hdr(r,"Cache-Control","no-store");
        httpd_resp_set_hdr(r,"X-Frame-Options","DENY");
        return httpd_resp_send(r,html_start,html_end-html_start);
    }
    cJSON *body = cJSON_CreateObject();
    if (!strcmp(r->uri,"/api/status")) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(station,&ip);
        char ip_text[16];
        snprintf(ip_text,sizeof(ip_text),IPSTR,IP2STR(&ip.ip));
        cJSON_AddBoolToObject(body,"connected",ip.ip.addr != 0);
        cJSON_AddStringToObject(body,"ip",ip_text);
        cJSON_AddStringToObject(body,"ssid",saved_ssid);
        cJSON_AddStringToObject(body,"zip",postal);
        cJSON_AddNumberToObject(body,"volume",volume);
        cJSON_AddNumberToObject(body,"gain",gain);
        cJSON_AddBoolToObject(body,"key_set",credentials_present());
        cJSON_AddBoolToObject(body,"key_saved",credentials_saved());
        cJSON_AddBoolToObject(body,"secure_storage",credentials_secure_storage());
        cJSON_AddBoolToObject(body,"active",live_voice_active());
        cJSON_AddStringToObject(body,"voice_status",live_voice_status());
        cJSON_AddBoolToObject(body,"finalized",live_voice_finalized());
        cJSON_AddNumberToObject(body,"usage_seconds",live_voice_usage_seconds());
        cJSON_AddItemToObject(body,"wake",wake_word_status());
        cJSON_AddItemToObject(body,"failure",live_voice_failure_status());
    } else if (!strcmp(r->uri,"/api/scan")) {
        if (esp_wifi_scan_start(NULL,true) != ESP_OK) { cJSON_Delete(body); return error(r,"Scan unavailable while connecting. Try again shortly."); }
        wifi_ap_record_t records[24]; uint16_t count = 24;
        esp_wifi_scan_get_ap_records(&count,records);
        cJSON *networks = cJSON_AddArrayToObject(body,"networks");
        for (int i=0;i<count;i++) {
            cJSON *network=cJSON_CreateObject();
            cJSON_AddStringToObject(network,"ssid",(char *)records[i].ssid);
            cJSON_AddNumberToObject(network,"rssi",records[i].rssi);
            cJSON_AddBoolToObject(network,"open",records[i].authmode == WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(networks,network);
        }
    } else {
        if (r->content_len <= 0 || r->content_len > 1024) { cJSON_Delete(body); return error(r,"Invalid request size"); }
        char buffer[1025]; size_t done=0;
        while (done<r->content_len) {
            int n=httpd_req_recv(r,buffer+done,r->content_len-done);
            if(n<=0){cJSON_Delete(body);return ESP_FAIL;} done+=n;
        }
        buffer[done]=0;
        cJSON *input=cJSON_Parse(buffer);
        if (!input) { cJSON_Delete(body); return error(r,"Invalid JSON"); }
        esp_err_t result=ESP_OK;
        if (!strcmp(r->uri,"/api/key")) {
            cJSON *key=cJSON_GetObjectItem(input,"key");
            if(live_voice_active()||!cJSON_IsString(key)) result=ESP_ERR_INVALID_STATE;
            else result=credentials_set(key->valuestring);
            if(cJSON_IsString(key))memset(key->valuestring,0,strlen(key->valuestring));
        } else if (!strcmp(r->uri,"/api/wake")) {
            cJSON *test=cJSON_GetObjectItem(input,"testing");
            if(cJSON_IsBool(test))result=wake_word_test(cJSON_IsTrue(test));
            else {
                cJSON *e=cJSON_GetObjectItem(input,"enabled"),*p=cJSON_GetObjectItem(input,"phrase"),*s=cJSON_GetObjectItem(input,"sensitivity");
                if(!cJSON_IsBool(e)||!cJSON_IsNumber(p)||!cJSON_IsNumber(s)||p->valuedouble!=p->valueint||s->valuedouble!=s->valueint)result=ESP_ERR_INVALID_ARG;
                else result=wake_word_configure(cJSON_IsTrue(e),p->valueint,s->valueint);
            }
        } else if (!strcmp(r->uri,"/api/voice")) {
            cJSON *action=cJSON_GetObjectItem(input,"action");
            if(!cJSON_IsString(action))result=ESP_ERR_INVALID_ARG;
            else if(!strcmp(action->valuestring,"start"))result=live_voice_start();
            else if(!strcmp(action->valuestring,"stop"))live_voice_stop();
            else if(!strcmp(action->valuestring,"tone"))audio_tone();
            else result=ESP_ERR_INVALID_ARG;
        } else if (!strcmp(r->uri,"/api/preferences")) {
            cJSON *v=cJSON_GetObjectItem(input,"volume"), *g=cJSON_GetObjectItem(input,"gain"), *z=cJSON_GetObjectItem(input,"zip");
            if(!cJSON_IsNumber(v)||v->valuedouble<0||v->valuedouble>80||!cJSON_IsNumber(g)||g->valuedouble<0||g->valuedouble>36||!cJSON_IsString(z)||strlen(z->valuestring)>16) result=ESP_ERR_INVALID_ARG;
            else {
                volume=v->valueint; gain=g->valueint; strlcpy(postal,z->valuestring,sizeof(postal));
                result=board_audio_volume(volume);
                if(result==ESP_OK) result=board_audio_gain(gain);
                if(result==ESP_OK) result=nvs_set_i32(storage,"volume",volume);
                if(result==ESP_OK) result=nvs_set_i32(storage,"gain",gain);
                if(result==ESP_OK) result=nvs_set_str(storage,"zip",postal);
                if(result==ESP_OK) result=nvs_commit(storage);
            }
        } else if (!strcmp(r->uri,"/api/wifi")) {
            cJSON *s=cJSON_GetObjectItem(input,"ssid"), *p=cJSON_GetObjectItem(input,"password"), *o=cJSON_GetObjectItem(input,"open");
            if(!cJSON_IsString(s)||strlen(s->valuestring)<1||strlen(s->valuestring)>32||!cJSON_IsString(p)||strlen(p->valuestring)>63) result=ESP_ERR_INVALID_ARG;
            else {
                wifi_config_t config={0};
                config.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
                config.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
                char password[64]; load_string("wifi_password",password,sizeof(password));
                if(cJSON_IsTrue(o)) password[0]=0;
                else if(p->valuestring[0]) strlcpy(password,p->valuestring,sizeof(password));
                else if(strcmp(s->valuestring,saved_ssid)) password[0]=0;
                if(!cJSON_IsTrue(o)&&strlen(password)<8) result=ESP_ERR_INVALID_ARG;
                else {
                    memcpy(config.sta.ssid,s->valuestring,strlen(s->valuestring));
                    memcpy(config.sta.password,password,strlen(password));
                    esp_wifi_disconnect();
                    result=esp_wifi_set_config(WIFI_IF_STA,&config);
                    if(result==ESP_OK) result=nvs_set_str(storage,"ssid",s->valuestring);
                    if(result==ESP_OK) result=nvs_set_str(storage,"wifi_password",password);
                    if(result==ESP_OK) result=nvs_commit(storage);
                    if(result==ESP_OK) {strlcpy(saved_ssid,s->valuestring,sizeof(saved_ssid)); result=esp_wifi_connect();}
                }
                memset(password,0,sizeof(password));
            }
        } else result=ESP_ERR_NOT_SUPPORTED;
        cJSON_Delete(input); memset(buffer,0,sizeof(buffer));
        if(result!=ESP_OK) {cJSON_Delete(body);return error(r,"Could not apply settings. Check the fields and try again.");}
        cJSON_AddBoolToObject(body,"ok",true);
        if(!strcmp(r->uri,"/api/key"))cJSON_AddBoolToObject(body,"key_saved",credentials_saved());
    }
    return json_response(r,body);
}
void setup_portal_enable(void) {
    if(ap_enabled) return;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t config={0};
    strlcpy((char *)config.ap.ssid,ap_name,sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password,ap_password,sizeof(config.ap.password));
    config.ap.authmode=WIFI_AUTH_WPA2_PSK; config.ap.max_connection=2;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&config));
    ap_enabled=true;
}
void setup_portal_init(void) {
    // Ordinary settings stay in their existing partition. Never auto-provision eFuses.
    ESP_ERROR_CHECK(nvs_flash_init_partition("nvs"));
    ESP_ERROR_CHECK(nvs_open("home_voice",NVS_READWRITE,&storage));
    load_string("setup_password",ap_password,sizeof(ap_password));
    if(!ap_password[0]) {
        snprintf(ap_password,sizeof(ap_password),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
        ESP_ERROR_CHECK(nvs_set_str(storage,"setup_password",ap_password));
        ESP_ERROR_CHECK(nvs_commit(storage));
    }
    load_string("ssid",saved_ssid,sizeof(saved_ssid)); load_string("zip",postal,sizeof(postal));
    nvs_get_i32(storage,"volume",&volume); nvs_get_i32(storage,"gain",&gain);
    if(volume<0||volume>80)volume=40;
    if(gain<0||gain>36)gain=24;
    ESP_ERROR_CHECK(board_audio_volume(volume)); ESP_ERROR_CHECK(board_audio_gain(gain));
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    station=esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_timer_create_args_t retry={.callback=reconnect_wifi,.name="wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&retry,&reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,mac));
    snprintf(ap_name,sizeof(ap_name),"HomeVoice-%02X%02X",mac[4],mac[5]);
    ESP_ERROR_CHECK(esp_wifi_start());
    if(!saved_ssid[0]) setup_portal_enable();
    else {
        wifi_config_t config={0}; char password[64]; load_string("wifi_password",password,sizeof(password));
        config.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
        memcpy(config.sta.ssid,saved_ssid,strlen(saved_ssid)); memcpy(config.sta.password,password,strlen(password));
        select_startup_access_point(&config);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&config)); esp_wifi_connect(); memset(password,0,sizeof(password));
    }
    httpd_handle_t server; httpd_config_t http=HTTPD_DEFAULT_CONFIG(); http.stack_size=8192;
    ESP_ERROR_CHECK(httpd_start(&server,&http));
    const char *paths[]={"/","/api/status","/api/scan","/api/preferences","/api/wifi","/api/key","/api/voice","/api/wake"};
    for(int i=0;i<8;i++) {httpd_uri_t route={.uri=paths[i],.method=i<3?HTTP_GET:HTTP_POST,.handler=handle};ESP_ERROR_CHECK(httpd_register_uri_handler(server,&route));}
}
