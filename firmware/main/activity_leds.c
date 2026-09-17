#include "activity_leds.h"
#include "live_voice.h"
#include "wake_word.h"
#include "setup_portal.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <math.h>

// This board revision uses RGB byte order on its seven GPIO38 LEDs.
enum { LED_COUNT = 7, SYMBOL_COUNT = LED_COUNT * 24 + 1 };
static atomic_uint audio_peak, wake_until;
static atomic_uint rendered_frames, rendered_audio;
unsigned activity_leds_frames(void) { return atomic_load(&rendered_frames); }
unsigned activity_leds_audio_frames(void) { return atomic_load(&rendered_audio); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

void activity_leds_wake(void) { atomic_store(&wake_until, now_ms()+800); }
void activity_leds_audio(const int16_t *samples, size_t count) {
    // Keep the audio task independent of the LED driver and its refresh rate.
    unsigned peak=0;
    for(size_t i=0;i<count;i++) {
        int value=samples[i]; if(value<0)value=-value;
        if((unsigned)value>peak)peak=value;
    }
    unsigned previous=atomic_load(&audio_peak);
    while(peak>previous && !atomic_compare_exchange_weak(&audio_peak,&previous,peak)) {}
}
static void worker(void *unused) {
    rmt_channel_handle_t channel=NULL;
    rmt_encoder_handle_t encoder=NULL;
    rmt_tx_channel_config_t config={.gpio_num=38,.clk_src=RMT_CLK_SRC_DEFAULT,
        .resolution_hz=10000000,.mem_block_symbols=256,.trans_queue_depth=1,
        .flags.with_dma=true};
    rmt_copy_encoder_config_t copy={0};
    // An unavailable indicator must never prevent the assistant from working.
    if(rmt_new_tx_channel(&config,&channel)!=ESP_OK)goto done;
    if(rmt_new_copy_encoder(&copy,&encoder)!=ESP_OK)goto done;
    if(rmt_enable(channel)!=ESP_OK)goto done;
    static rmt_symbol_word_t symbols[SYMBOL_COUNT];
    rmt_transmit_config_t tx={0};
    float envelope=0;
    bool was_idle_failed=false;
    uint32_t error_started=0;
    for(;;) {
        unsigned peak=atomic_exchange(&audio_peak,0);
        if(setup_portal_volume()==0)peak=0;
        float level=peak>180?fminf(1.0f,sqrtf(peak/18000.0f)):0;
        envelope=fmaxf(level,envelope*0.78f);
        if(envelope<0.03f)envelope=0;
        uint32_t now=now_ms(),until=atomic_load(&wake_until);
        bool waking=until && (int32_t)(until-now)>0;
        bool active=live_voice_active(), listening=live_voice_ready();
        bool armed=wake_word_listening();
        bool idle_failed=!active && live_voice_failed();
        if(idle_failed && !was_idle_failed)error_started=now;
        was_idle_failed=idle_failed;
        // A retained failure is history, not an ongoing busy state. Give it a
        // brief indication, then show wake readiness without clearing diagnostics.
        bool show_error=idle_failed && (uint32_t)(now-error_started)<3000;
        for(unsigned i=0;i<LED_COUNT;i++) {
            uint8_t r=0,g=0,b=0;
            if(waking) {g=45;}
            else if(envelope>0) {
                float fill=fminf(1.0f,fmaxf(0,envelope*LED_COUNT-i));
                g=(uint8_t)(22*fill);b=(uint8_t)(64*fill);r=(uint8_t)(8*fill);
            } else if(active && !listening) {
                if(i==(now/140)%LED_COUNT){r=35;g=14;}
            } else if(active) {g=8;b=12;}
            else if(show_error) {if(((now-error_started)/500)%2==0)r=20;}
            else if(armed && i==0) {g=2;b=3;}
            uint8_t rgb[]={r,g,b};
            for(unsigned byte=0;byte<3;byte++)for(unsigned bit=0;bit<8;bit++) {
                bool one=(rgb[byte] & (0x80>>bit))!=0;
                symbols[i*24+byte*8+bit]=(rmt_symbol_word_t){
                    .level0=1,.duration0=one?9:3,.level1=0,.duration1=one?3:9};
            }
        }
        symbols[SYMBOL_COUNT-1]=(rmt_symbol_word_t){.duration0=1500,.duration1=1500};
        // DMA handles the waveform; only this low-priority task waits for it.
        if(rmt_transmit(channel,encoder,symbols,sizeof(symbols),&tx)!=ESP_OK)break;
        if(rmt_tx_wait_all_done(channel,100)!=ESP_OK)break;
        atomic_fetch_add(&rendered_frames,1);
        if(envelope>0 && !waking)atomic_fetch_add(&rendered_audio,1);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
    rmt_disable(channel);
done:
    if(encoder)rmt_del_encoder(encoder);
    if(channel)rmt_del_channel(channel);
    vTaskDelete(NULL);
}
void activity_leds_init(void) { xTaskCreate(worker,"activity_leds",3072,NULL,1,NULL); }
