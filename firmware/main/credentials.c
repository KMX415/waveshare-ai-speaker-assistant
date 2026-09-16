#include "credentials.h"
#include <string.h>
#include "esp_efuse.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Provisioning is only reachable through the explicit USB command authorized by
// the owner. Boot and web requests never generate or program hardware secrets.
#define KEY_BLOCK EFUSE_BLK_KEY5
#define OWNER_MAGIC 0x48564b32
static char api_key[513];
static bool saved, secure;
static SemaphoreHandle_t lock;
static nvs_handle_t vault;
bool credentials_secure_storage(void) { return secure; }
bool credentials_saved(void) { return saved; }
bool credentials_present(void) {
    xSemaphoreTake(lock,portMAX_DELAY);bool present=api_key[0]!=0;xSemaphoreGive(lock);return present;
}
void credentials_copy(char out[513]) {
    xSemaphoreTake(lock,portMAX_DELAY);memcpy(out,api_key,sizeof(api_key));xSemaphoreGive(lock);
}
void credentials_init(void) {
    lock=xSemaphoreCreateMutex();configASSERT(lock);
    // nvs_flash_init() can auto-burn a key. Explicit partition init cannot.
    ESP_ERROR_CHECK(nvs_flash_init_partition("nvs"));
    nvs_handle_t meta;uint32_t marker=0;
    if(nvs_open("voice_security",NVS_READONLY,&meta)!=ESP_OK)return;
    nvs_get_u32(meta,"owner",&marker);nvs_close(meta);
    if(marker!=OWNER_MAGIC || esp_efuse_get_key_purpose(KEY_BLOCK)!=ESP_EFUSE_KEY_PURPOSE_HMAC_UP ||
       !esp_efuse_get_key_dis_read(KEY_BLOCK))return;
    nvs_sec_scheme_t *scheme=NULL;
    nvs_sec_config_hmac_t hmac={.hmac_key_id=HMAC_KEY5};
    if(nvs_sec_provider_register_hmac(&hmac,&scheme)!=ESP_OK)return;
    nvs_sec_cfg_t cfg={0};
    esp_err_t err=nvs_flash_read_security_cfg_v2(scheme,&cfg);
    if(err==ESP_OK)err=nvs_flash_secure_init_partition("credentials",&cfg);
    mbedtls_platform_zeroize(&cfg,sizeof(cfg));
    nvs_sec_provider_deregister(scheme);
    if(err==ESP_OK)err=nvs_open_from_partition("credentials","openai",NVS_READWRITE,&vault);
    secure=err==ESP_OK;
    if(!secure)return;
    size_t length=sizeof(api_key);
    saved=nvs_get_str(vault,"api_key",api_key,&length)==ESP_OK && length>20;
    if(!saved)mbedtls_platform_zeroize(api_key,sizeof(api_key));
}
esp_err_t credentials_set(const char *key) {
    size_t length=strlen(key);
    if(length<20 || length>512)return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<length;i++)if(key[i]<33 || key[i]>126)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t err=ESP_ERR_INVALID_STATE;
    if(secure) {
        err=nvs_set_str(vault,"api_key",key);
        if(err==ESP_OK)err=nvs_commit(vault);
    }
    // Never report a successful save when storage is only volatile.
    if(err==ESP_OK) {
        mbedtls_platform_zeroize(api_key,sizeof(api_key));memcpy(api_key,key,length);saved=true;
    }
    xSemaphoreGive(lock);return err;
}

esp_err_t credentials_provision(void) {
    if(secure)return ESP_OK;
    // Never overwrite or borrow a key installed by another application.
    if(!esp_efuse_key_block_unused(KEY_BLOCK))return ESP_ERR_INVALID_STATE;
    nvs_handle_t meta;
    esp_err_t err=nvs_open("voice_security",NVS_READWRITE,&meta);
    if(err!=ESP_OK)return err;
    err=nvs_set_u32(meta,"owner",OWNER_MAGIC);
    if(err==ESP_OK)err=nvs_commit(meta);
    nvs_close(meta);
    if(err!=ESP_OK)return err;
    // Wi-Fi is running, supplying entropy to the hardware RNG.
    uint8_t secret[32];esp_fill_random(secret,sizeof(secret));
    err=esp_efuse_write_key(KEY_BLOCK,ESP_EFUSE_KEY_PURPOSE_HMAC_UP,secret,sizeof(secret));
    mbedtls_platform_zeroize(secret,sizeof(secret));
    return err;
}
