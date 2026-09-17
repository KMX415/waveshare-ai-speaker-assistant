#include "mic_control.h"
#include "setup_portal.h"
#include "live_voice.h"
#include "esp_vad.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { IDLE, WAIT_NOISE, NOISE, WAIT_SPEECH, SPEECH, WAIT_VERIFY, VERIFY };
typedef struct {unsigned frames,voiced,peak,clipped;double raw_power,clean_power;} measurement;
static portMUX_TYPE stats_lock=portMUX_INITIALIZER_UNLOCKED;
static measurement stats;
static atomic_int stage;
static atomic_int result; // 0 none/in progress, 1 verified/saved, -1 unsuccessful
static atomic_int original_gain,candidate_gain;
static atomic_int noise_db=-96,speech_db=-96,snr_db,correction_tenths;
static unsigned stage_ms,prompt_generation,quiet_ms;
static bool prompt_heard,old_auto,tentative;
static double baseline_power=1;
static vad_handle_t vad;
static int16_t frame[160],raw_frame[160],output[160];
static unsigned filled;
static float correction=1.0f,noise_floor=100.0f;
static unsigned now_ms(void){return (unsigned)(esp_timer_get_time()/1000);}
static float db(double power){return 10.0f*log10f(fmaxf((float)power,1.0f)/(32768.0f*32768.0f));}
static measurement snapshot(void){portENTER_CRITICAL(&stats_lock);measurement s=stats;portEXIT_CRITICAL(&stats_lock);return s;}
static void reset_stats(void){portENTER_CRITICAL(&stats_lock);memset(&stats,0,sizeof(stats));portEXIT_CRITICAL(&stats_lock);}
bool mic_control_active(void){return atomic_load(&stage)!=IDLE;}
void mic_control_init(void){vad=vad_create_with_param(VAD_MODE_3,16000,10,100,100);}

// One 10 ms processing delay lets the existing 16 ms AEC/upload framing stay intact.
void mic_control_process(int16_t *clean,const int16_t *raw,size_t count) {
    for(size_t i=0;i<count;i++) {
        frame[filled]=clean[i];raw_frame[filled]=raw[i];clean[i]=output[filled];
        if(++filled!=160)continue;
        filled=0;
        double rp=0,cp=0;unsigned peak=0,clipped=0;
        for(unsigned n=0;n<160;n++){
            int r=raw_frame[n],c=frame[n],a=r<0?-r:r;
            rp+=(double)r*r;cp+=(double)c*c;
            if((unsigned)a>peak)peak=a;
            if(a>=32700)clipped++;
        }
        rp/=160;cp/=160;
        bool playing=audio_is_playing();
        bool voice=vad&&vad_process(vad,frame,16000,10)==VAD_SPEECH;
        int phase=atomic_load(&stage);
        if(!playing && (phase==NOISE || phase==SPEECH || phase==VERIFY)) {
            portENTER_CRITICAL(&stats_lock);
            if(phase==NOISE || voice){
                stats.frames++;stats.voiced+=voice;stats.raw_power+=rp;stats.clean_power+=cp;
                if(peak>stats.peak)stats.peak=peak;
                stats.clipped+=clipped;
            }
            portEXIT_CRITICAL(&stats_lock);
        }
        float rms=sqrtf((float)cp);
        if(!playing&&!voice)noise_floor=.99f*noise_floor+.01f*rms;
        float target=1;
        if(setup_portal_auto_gain() && phase==IDLE && !playing && voice && rms>fmaxf(100,noise_floor*2))
            target=fminf(2.0f,fmaxf(.5f,1800.0f/rms));
        if(phase!=IDLE || !setup_portal_auto_gain())correction=1;
        else correction+=(target-correction)*(target<correction?.20f:.03f);
        atomic_store(&correction_tenths,(int)lroundf(200*log10f(correction)));
        for(unsigned n=0;n<160;n++) {
            float sample=frame[n]*correction;
            // Leave the original signal untouched when auto gain is off or calibrating.
            if(setup_portal_auto_gain()&&phase==IDLE)sample=fmaxf(-30000,fminf(30000,sample));
            output[n]=(int16_t)sample;
        }
    }
}
static int recommend(measurement s,double noise,int current) {
    if(s.frames<100)return -1; // At least one second of detected speech.
    double speech=s.clean_power/s.frames;
    if(10*log10(fmax(speech,1)/fmax(noise,1))<10)return -1;
    float change=-24-db(s.raw_power/s.frames);
    float headroom=20*log10f(16384.0f/fmaxf(s.peak,1));
    if(change>headroom)change=headroom;
    if(s.clipped)change=-6;
    int steps=(int)floorf(change/3);
    if(steps>2)steps=2;
    if(steps<-2)steps=-2;
    int gain=current+steps*3;
    if(gain>30&&gain<36)gain=30; // Codec has no 33 dB setting.
    return gain<0?0:(gain>36?36:gain);
}
static bool verified(measurement s,double noise) {
    if(s.frames<100)return false;
    float level=db(s.raw_power/s.frames);
    return level>=-40 && level<=-12 && s.clipped*1000u<s.frames*160u &&
        10*log10(fmax(s.clean_power/s.frames,1)/fmax(noise,1))>=10;
}
static void enter_wait(int phase) {
    reset_stats();stage_ms=now_ms();quiet_ms=0;prompt_heard=false;
    prompt_generation=audio_output_generation();atomic_store(&stage,phase);
}
esp_err_t mic_control_start(void) {
    if(!vad || !live_voice_ready() || mic_control_active())return ESP_ERR_INVALID_STATE;
    original_gain=setup_portal_gain();candidate_gain=original_gain;old_auto=setup_portal_auto_gain();
    tentative=false;result=0;noise_db=-96;speech_db=-96;snr_db=0;
    enter_wait(WAIT_NOISE);return ESP_OK;
}
void mic_control_cancel(void) {
    if(!mic_control_active())return;
    if(tentative)setup_portal_set_microphone(original_gain,old_auto,false);
    tentative=false;result=-1;stage=IDLE;
}
static void abort_calibration(const char *why) {
    mic_control_cancel();
    live_voice_instruction(why);
}
void mic_control_tick(void) {
    int phase=atomic_load(&stage);
    if(phase==IDLE)return;
    if(!live_voice_ready()){mic_control_cancel();return;}
    unsigned now=now_ms();
    if(phase==WAIT_NOISE||phase==WAIT_SPEECH||phase==WAIT_VERIFY) {
        if(audio_output_generation()!=prompt_generation)prompt_heard=true;
        if(audio_is_playing())quiet_ms=0;
        else if(!quiet_ms)quiet_ms=now;
        if(prompt_heard && quiet_ms && now-quiet_ms>=700) {
            reset_stats();stage_ms=now;
            stage=phase==WAIT_NOISE?NOISE:(phase==WAIT_SPEECH?SPEECH:VERIFY);
        } else if(now-stage_ms>25000)abort_calibration("Microphone calibration stopped because the spoken prompt could not be completed. Tell the user nothing was saved, and resume normal conversation.");
        return;
    }
    measurement s=snapshot();
    if(phase==NOISE) {
        if(s.frames<300) {
            if(now-stage_ms>10000)abort_calibration("Microphone calibration could not measure a quiet room sample. Nothing was saved. Ask the user to try again with the speaker and room quiet. Resume normal conversation.");
            return;
        }
        if(s.voiced>s.frames/10 || s.clipped) {
            abort_calibration("Microphone calibration heard speech or excessive noise during the quiet sample. Nothing was saved. Ask the user to retry and remain quiet for the first step. Resume normal conversation.");return;
        }
        baseline_power=fmax(s.clean_power/s.frames,1);noise_db=(int)lround(db(baseline_power));
        enter_wait(WAIT_SPEECH);
        if(!live_voice_instruction("Microphone calibration: the quiet sample is complete. Say only: From your usual kitchen spot, speak normally for about five seconds after I finish. Try: Jarvis, can you hear me clearly from this part of the kitchen? Then remain silent yourself until the next calibration update. Do not answer or memorize the user's sample phrase."))mic_control_cancel();
    } else if(now-stage_ms>=10000) {
        if(phase==SPEECH) {
            int chosen=recommend(s,baseline_power,original_gain);
            if(chosen<0){abort_calibration("Microphone calibration could not separate enough clear speech from room noise. Nothing was saved. Suggest moving the speaker away from appliances or speaking closer and trying again. Resume normal conversation.");return;}
            speech_db=(int)lround(db(s.raw_power/s.frames));snr_db=(int)lround(10*log10(fmax(s.clean_power/s.frames,1)/baseline_power));
            if(setup_portal_set_microphone(chosen,false,false)!=ESP_OK){abort_calibration("Microphone adjustment failed; calibration was not saved. Resume normal conversation.");return;}
            tentative=true;candidate_gain=chosen;enter_wait(WAIT_VERIFY);
            if(!live_voice_instruction("Microphone calibration: a candidate gain is being tested, not saved yet. Ask the user to repeat the same kitchen phrase at their normal volume from the same spot after you finish. Then remain silent until the calibration result arrives. Do not answer or memorize the test phrase."))mic_control_cancel();
        } else {
            double adjusted_noise=baseline_power*pow(10,(candidate_gain-original_gain)/10.0);
            if(!verified(s,adjusted_noise)){abort_calibration("Microphone verification did not get a clear, unclipped voice sample. The previous setting has been restored; nothing new was saved. Suggest retrying with less appliance noise or a closer position. Resume normal conversation.");return;}
            // Put the baseline back before committing so the setter's rollback
            // target is the saved baseline rather than the temporary candidate.
            if(setup_portal_set_microphone(original_gain,old_auto,false)!=ESP_OK || setup_portal_set_microphone(candidate_gain,true,true)!=ESP_OK){abort_calibration("Microphone settings could not be saved. The previous gain has been restored. Resume normal conversation.");return;}
            tentative=false;result=1;stage=IDLE;
            speech_db=(int)lround(db(s.raw_power/s.frames));snr_db=(int)lround(10*log10(fmax(s.clean_power/s.frames,1)/adjusted_noise));
            char message[420];snprintf(message,sizeof(message),"Microphone calibration verified and saved on the device. Hardware gain is %d dB (previously %d). Measured speech was %d dBFS with estimated speech-to-noise margin %d dB, without significant clipping. Bounded automatic gain is enabled. Briefly tell the user calibration passed and settings survive unplugging. Do not claim all distances or noise conditions are guaranteed. Resume normal conversation.",(int)candidate_gain,(int)original_gain,(int)speech_db,(int)snr_db);
            live_voice_instruction(message);
        }
    }
}
cJSON *mic_control_status(void) {
    static const char *names[]={"idle","waiting_for_quiet_prompt","measuring_room","waiting_for_speech_prompt","measuring_speech","waiting_for_verification_prompt","verifying"};
    cJSON *j=cJSON_CreateObject();int phase=atomic_load(&stage);
    cJSON_AddStringToObject(j,"type","microphone_status");cJSON_AddStringToObject(j,"phase",names[phase]);
    cJSON_AddBoolToObject(j,"active",phase!=IDLE);cJSON_AddBoolToObject(j,"available",vad!=NULL);
    cJSON_AddNumberToObject(j,"gain_db",setup_portal_gain());cJSON_AddBoolToObject(j,"automatic",setup_portal_auto_gain());
    cJSON_AddNumberToObject(j,"correction_db",atomic_load(&correction_tenths)/10.0);
    cJSON_AddNumberToObject(j,"result",atomic_load(&result));
    cJSON_AddNumberToObject(j,"noise_dbfs",atomic_load(&noise_db));cJSON_AddNumberToObject(j,"speech_dbfs",atomic_load(&speech_db));cJSON_AddNumberToObject(j,"snr_db",atomic_load(&snr_db));
    return j;
}
unsigned mic_control_selftest(void) {
    unsigned passed=0;
    measurement s={.frames=200,.voiced=200,.peak=4000,.raw_power=200*1000000.0,.clean_power=200*1000000.0};
    if(recommend(s,10000,24)==30)passed|=1;
    if(verified(s,10000))passed|=2;
    if(recommend(s,500000,24)==-1)passed|=4;
    s.frames=20;if(recommend(s,10000,24)==-1)passed|=8;
    s.frames=200;s.clipped=100;if(recommend(s,10000,24)==18&&!verified(s,10000))passed|=16;
    s.clipped=0;s.peak=30000;if(recommend(s,10000,24)<24)passed|=32;
    s.peak=4000;if(recommend(s,10000,36)==36)passed|=64;
    s.clipped=100;if(recommend(s,10000,0)==0)passed|=128;
    return passed;
}
