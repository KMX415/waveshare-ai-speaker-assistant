#include "device_identity.h"
#include "esp_mac.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
static nvs_handle_t storage;
static SemaphoreHandle_t mutex;
static char device_id[13],room_name[33];
static bool room_valid(const char *room) {
    if(!room||!room[0]||strlen(room)>32||room[0]==' '||room[strlen(room)-1]==' ')return false;
    for(const unsigned char *p=(const unsigned char*)room;*p;p++)if(*p<32||*p==127)return false;
    return true;
}
void device_identity_init(void) {
    mutex=xSemaphoreCreateMutex();configASSERT(mutex);
    uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    snprintf(device_id,sizeof(device_id),"%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    ESP_ERROR_CHECK(nvs_open("device_identity",NVS_READWRITE,&storage));
    size_t size=sizeof(room_name);
    if(nvs_get_str(storage,"room",room_name,&size)!=ESP_OK||!room_valid(room_name))snprintf(room_name,sizeof(room_name),"Speaker %.4s",device_id+8);
}
esp_err_t device_identity_set_room(const char *room) {
    if(!mutex||!room_valid(room))return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex,portMAX_DELAY);
    esp_err_t result=nvs_set_str(storage,"room",room);
    if(result==ESP_OK)result=nvs_commit(storage);
    if(result==ESP_OK)snprintf(room_name,sizeof(room_name),"%s",room);
    xSemaphoreGive(mutex);return result;
}
cJSON *device_identity_status(void) {
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"type","device_identity");
    if(mutex){xSemaphoreTake(mutex,portMAX_DELAY);cJSON_AddStringToObject(j,"device_id",device_id);cJSON_AddStringToObject(j,"room",room_name);xSemaphoreGive(mutex);}
    return j;
}
