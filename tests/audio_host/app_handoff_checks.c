/* 编译 app_main 生产函数；播放/平台打断切换不能停止、清空上行。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "../../main/audio_in.h"
#include "../../main/ws_service.h"
typedef void *esp_timer_handle_t;
#include "../../main/app_state.h"
static unsigned checks, accepts, recoveries, resumes, timer_pauses;
static bool conversation_active, offered, fail_accept, interrupted;
static int64_t response_deadline_ms;
static app_state_t current_state;
static audio_in_upload_status_t upload;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int64_t now_ms(void) { return 1000; }
static void recover_session(const char *reason) { CHECK(reason != NULL); ++recoveries; }
static void resume_listening(bool keep, const char *reason)
{ CHECK(keep && reason != NULL && upload.running); ++resumes; current_state=APP_STATE_LISTENING; }
app_state_t app_state_get(void) { return current_state; }
app_state_t app_state_set(app_state_t s) { app_state_t old=current_state; current_state=s; return old; }
void app_state_pause_silence_timer(void) { ++timer_pauses; }
audio_in_handle_t audio_in_get_handle(void) { return (void*)1; }
void audio_in_get_upload_status(audio_in_handle_t h, audio_in_upload_status_t *s)
{ CHECK(h == (void*)1); *s=upload; }
bool ws_service_get_reply_offer(ws_reply_offer_t *o)
{ o->generation=7; o->turn=2; return offered; }
bool ws_service_accept_reply(const ws_reply_offer_t *o)
{ CHECK(o->generation==7 && o->turn==2 && upload.running); if(fail_accept) return false; ++accepts; offered=false; return true; }
bool ws_service_reply_interrupted(void) { return interrupted; }
#include "app_handoff.inc"
int main(void)
{
    conversation_active=offered=true; upload.running=true; current_state=APP_STATE_LISTENING;
    upload.sent_end=320; upload.speech_end=48000;
    process_reply_offer(); CHECK(accepts==1 && current_state==APP_STATE_PLAYING);
    CHECK(timer_pauses==1 && response_deadline_ms==46000 && upload.running);
    process_reply_offer(); CHECK(accepts==1);
    interrupted=true; process_platform_interruption();
    CHECK(resumes==1 && current_state==APP_STATE_LISTENING && upload.running);
    interrupted=false; current_state=APP_STATE_PLAYING;
    process_platform_interruption(); CHECK(resumes==1 && current_state==APP_STATE_PLAYING);
    offered=fail_accept=true; current_state=APP_STATE_LISTENING;
    process_reply_offer(); CHECK(accepts==1 && current_state==APP_STATE_LISTENING);
    upload.running=false; process_reply_offer(); CHECK(recoveries==1);
    conversation_active=false; interrupted=true;
    process_platform_interruption(); process_reply_offer(); CHECK(recoveries==1 && resumes==1);
    printf("PASS: %u app platform handoff checks\n",checks); return 0;
}
