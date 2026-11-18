/* 编译主任务的生产提醒选择/退出路径；网络、音频硬件和 MQTT 所有权用桩替换。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "../../main/audio_in.h"
#include "../../main/ws_service.h"
#include "../../main/wifi_service.h"
#include "../../main/wake_word.h"
#include "../../main/device_mqtt_service.h"
typedef void *esp_timer_handle_t;
#include "../../main/app_state.h"

static unsigned checks, takes, stops, closes, clears, finishes, pauses;
static bool conversation_active, session_requested, reminder_requested;
static bool voice_auth_refresh_used, awaiting_voice_auth, waiting_for_credentials;
static bool provisioning, voice_enabled, dispatch_allowed, timed_out, audio_active;
static bool audio_buffer_enabled;
static int64_t clock_ms, reminder_deadline_ms, retry_at_ms, auth_deadline_ms;
static int64_t response_deadline_ms, resume_deadline_ms;
static device_mqtt_delivery_t batch, *reminder_task, *available;
static device_mqtt_task_result_t last_result;
static app_state_t current_state;
static wifi_state_t wifi_state;
static wake_word_mode_t afe_mode;
static esp_err_t afe_result;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int64_t now_ms(void) { return clock_ms; }
bool provisioning_service_is_active(void) { return provisioning; }
wifi_state_t wifi_service_get_state(void) { return wifi_state; }
esp_err_t wifi_service_set_audio_active(bool active) { audio_active=active; return ESP_OK; }
app_state_t app_state_get(void) { return current_state; }
app_state_t app_state_set(app_state_t s) { app_state_t old=current_state; current_state=s; return old; }
void app_state_pause_silence_timer(void) { ++pauses; }
esp_err_t wake_word_set_mode(wake_word_mode_t mode) { afe_mode=mode; return afe_result; }
void wake_word_clear_audio_buffer(void) { ++clears; }
void wake_word_set_audio_buffer_enabled(bool enabled) { audio_buffer_enabled=enabled; }
audio_in_handle_t audio_in_get_handle(void) { return (void*)1; }
esp_err_t audio_in_stop(audio_in_handle_t h) { CHECK(h==(void*)1); ++stops; audio_active=false; return ESP_OK; }
esp_err_t ws_service_disconnect(void) { ++closes; return ESP_OK; }
bool ws_service_reminder_timed_out(void) { return timed_out; }
void device_mqtt_service_set_dispatch_allowed(bool allowed) { dispatch_allowed=allowed; }
bool device_mqtt_service_voice_enabled(void) { return voice_enabled; }
device_mqtt_delivery_t *device_mqtt_service_take_tasks(void)
{ ++takes; device_mqtt_delivery_t *p=available; available=NULL; return p; }
void device_mqtt_service_finish_tasks(device_mqtt_delivery_t *p,device_mqtt_task_result_t result)
{ CHECK(p==&batch); ++finishes; last_result=result; }
static void led_set_waiting_wake(void) {}
static void led_set_wifi_err(void) {}
esp_err_t app_state_back_to_wait_wake(bool reconnect)
{ CHECK(!reconnect); current_state=APP_STATE_WAIT_WAKE; afe_mode=WAKE_WORD_MODE_WAKE; return ESP_OK; }
#include "app_reminder.inc"
static void reset(void)
{
    conversation_active=session_requested=reminder_requested=false;
    voice_auth_refresh_used=awaiting_voice_auth=waiting_for_credentials=false;
    provisioning=timed_out=audio_active=false; voice_enabled=dispatch_allowed=true;
    audio_buffer_enabled=true;
    reminder_task=NULL; available=&batch; batch.count=2;
    takes=stops=closes=clears=finishes=pauses=0;
    current_state=APP_STATE_WAIT_WAKE; wifi_state=WIFI_STATE_GOT_IP;
    afe_mode=WAKE_WORD_MODE_WAKE; afe_result=ESP_OK;
    clock_ms=1000; reminder_deadline_ms=retry_at_ms=auth_deadline_ms=0;
    response_deadline_ms=resume_deadline_ms=0;
}
int main(void)
{
    reset(); session_requested=true; poll_reminder(); CHECK(takes==0 && reminder_task==NULL);
    session_requested=false; provisioning=true; poll_reminder(); CHECK(takes==0);
    provisioning=false; wifi_state=WIFI_STATE_DISCONNECTED; poll_reminder(); CHECK(takes==0);
    wifi_state=WIFI_STATE_GOT_IP; current_state=APP_STATE_LISTENING; poll_reminder(); CHECK(takes==0);
    current_state=APP_STATE_WAIT_WAKE; voice_enabled=false; poll_reminder(); CHECK(takes==0);
    voice_enabled=true; poll_reminder();
    CHECK(takes==1 && reminder_task==&batch && session_requested && !conversation_active);
    CHECK(!reminder_requested && !dispatch_allowed && pauses==1 && audio_active);
    CHECK(afe_mode==WAKE_WORD_MODE_LITE && current_state==APP_STATE_REMINDER_WAITING);
    CHECK(!audio_buffer_enabled);
    CHECK(retry_at_ms==1000 && reminder_deadline_ms==361000 && finishes==0);
    poll_reminder(); CHECK(takes==1 && finishes==0);
    reminder_requested=true; timed_out=true; poll_reminder();
    CHECK(finishes==1 && last_result==DEVICE_TASK_FAILED && reminder_task==NULL);
    CHECK(!session_requested && !reminder_requested && !audio_active);
    CHECK(stops==1 && closes==1 && clears==1 && afe_mode==WAKE_WORD_MODE_WAKE);
    CHECK(current_state==APP_STATE_WAIT_WAKE && retry_at_ms==0 && reminder_deadline_ms==0);
    CHECK(audio_buffer_enabled);
    reset(); afe_result=ESP_FAIL; poll_reminder();
    CHECK(finishes==1 && last_result==DEVICE_TASK_FAILED && !session_requested && !audio_active);
    CHECK(audio_buffer_enabled);
    reset(); poll_reminder(); wifi_state=WIFI_STATE_DISCONNECTED;
    end_conversation("network lost"); CHECK(finishes==1 && last_result==DEVICE_TASK_NETWORK_WAIT);
    reset(); poll_reminder(); clock_ms=361000; poll_reminder();
    CHECK(finishes==1 && last_result==DEVICE_TASK_FAILED && !session_requested);
    printf("PASS: %u app reminder lifecycle checks\n",checks); return 0;
}
