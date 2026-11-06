/* 生产 app_main 交接函数由 check_audio.py 原样提取；只替换服务依赖。
 * 验证真实调用顺序：积压排空、暂停再检查、过时 offer、取消/恢复和有界等待。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "../../main/input_handoff.h"
#include "../../main/audio_in.h"
#include "../../main/wake_word.h"
#include "../../main/ws_service.h"
typedef void *esp_timer_handle_t;
#include "../../main/app_state.h"

static unsigned checks, pauses, stops, starts, cancels, accepts, recoveries;
static bool conversation_active, offered, paused, change_offer_on_pause, more_audio_on_pause;
static bool fail_accept, fail_pause, failed_input_on_pause;
static int64_t response_deadline_ms, test_now;
static app_state_t current_state;
static ws_reply_offer_t current_offer;
static audio_in_upload_status_t upload;
static input_handoff_speech_t fresh_speech;
static uint32_t observer_id, next_observer;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

static int64_t now_ms(void) { return test_now; }
static void recover_session(const char *reason);
static void resume_listening(bool keep, const char *reason);
static void interrupt_response(bool manual);
#include "app_handoff.inc"

static void recover_session(const char *reason)
{
    CHECK(reason != NULL); ++recoveries;
    reset_input_handoff(); offered = paused = upload.running = false;
    current_state = APP_STATE_WAIT_WAKE;
}
static void resume_listening(bool keep, const char *reason)
{
    CHECK(keep && reason); reset_input_handoff();
    paused = false; upload.running = true; current_state = APP_STATE_LISTENING;
}
static void interrupt_response(bool manual)
{
    CHECK(!manual && paused && upload.running); ++cancels;
    current_state = APP_STATE_CANCELLING; offered = false;
}
app_state_t app_state_get(void) { return current_state; }
app_state_t app_state_set(app_state_t state) { current_state = state; return state; }
void app_state_pause_silence_timer(void) {}
audio_in_handle_t audio_in_get_handle(void) { return (void *)1; }
void audio_in_get_upload_status(audio_in_handle_t h, audio_in_upload_status_t *status)
{ CHECK(h == (void *)1); *status = upload; }
esp_err_t audio_in_pause(audio_in_handle_t h)
{
    CHECK(h == (void *)1); ++pauses;
    if (fail_pause) return ESP_ERR_TIMEOUT;
    paused = true;
    if (change_offer_on_pause) ++current_offer.turn;
    if (more_audio_on_pause) upload.speech_end += 512;
    if (failed_input_on_pause) upload.running = false;
    return ESP_OK;
}
esp_err_t audio_in_start_from_ringbuf(audio_in_handle_t h, RingbufHandle_t rb)
{ CHECK(h == (void *)1 && rb == (void *)2); ++starts; paused = false; return ESP_OK; }
esp_err_t audio_in_stop(audio_in_handle_t h)
{
    CHECK(h == (void *)1); CHECK(paused || !upload.running);
    ++stops; upload.running = false; paused = false; return ESP_OK;
}
RingbufHandle_t wake_word_get_ringbuf(void) { return (void *)2; }
uint32_t wake_word_watch_input_handoff(void)
{ observer_id = ++next_observer; input_handoff_speech_reset(&fresh_speech); return observer_id; }
void wake_word_end_input_handoff(uint32_t id) { if (id == observer_id) observer_id = 0; }
bool wake_word_get_input_handoff(uint32_t id, bool *pending, bool *confirmed)
{
    *pending = fresh_speech.guard.samples != 0; *confirmed = fresh_speech.confirmed;
    return id && id == observer_id;
}
bool ws_service_get_reply_offer(ws_reply_offer_t *offer)
{ if (!offered) return false; *offer = current_offer; return true; }
bool ws_service_accept_reply(const ws_reply_offer_t *offer)
{
    CHECK(!upload.running && current_state == APP_STATE_PLAYING);
    CHECK(offer->generation == current_offer.generation && offer->turn == current_offer.turn);
    if (fail_accept) return false;
    ++accepts; offered = false; return true;
}
static void setup(unsigned lag_ms, bool replaying)
{
    reset_input_handoff();
    pauses = stops = starts = cancels = accepts = recoveries = 0;
    paused = change_offer_on_pause = more_audio_on_pause = false;
    fail_accept = fail_pause = failed_input_on_pause = false;
    conversation_active = offered = true; test_now = 1000;
    current_state = APP_STATE_LISTENING;
    ++current_offer.turn; current_offer.generation = 7;
    upload = (audio_in_upload_status_t){true, replaying, 48000 - lag_ms * 16, 48000};
}
int main(void)
{
    const unsigned lags[] = {32, 44, 1040, 1400};
    for (unsigned i = 0; i < 4; ++i) {
        setup(lags[i], false);
        process_reply_offer();
        CHECK(stops == 0 && pauses == 0 && cancels == 0 && upload.running);
        while (upload.sent_end < upload.speech_end) {
            upload.sent_end += 320; test_now += 20;
            process_reply_offer();
        }
        CHECK(accepts == 1 && stops == 1 && pauses == 1 && cancels == 0);
        CHECK(current_state == APP_STATE_PLAYING && !upload.running);
        process_reply_offer(); CHECK(accepts == 1); /* 不重复批准 */
    }
    setup(0, true);
    process_reply_offer(); CHECK(stops == 0 && cancels == 0);
    upload.replaying = false;
    process_reply_offer(); CHECK(accepts == 1 && cancels == 0);

    setup(32, false); process_reply_offer();
    /* 仍有新的人声：只有累计确认后才暂停取消。 */
    for (unsigned i = 0; i < 14; ++i) {
        input_handoff_speech_feed(&fresh_speech, true, 20000, 512);
        test_now += 32; process_reply_offer(); CHECK(cancels == 0);
    }
    input_handoff_speech_feed(&fresh_speech, true, 20000, 512);
    process_reply_offer();
    CHECK(cancels == 1 && stops == 0 && paused && upload.running);
    CHECK(current_state == APP_STATE_CANCELLING && !observer_id);
    process_reply_offer(); CHECK(cancels == 1);
    resume_listening(true, "cancel done");
    CHECK(upload.running && !paused && current_state == APP_STATE_LISTENING);

    setup(0, false); more_audio_on_pause = true;
    process_reply_offer(); CHECK(stops == 0 && starts == 1 && cancels == 0 && !paused);
    more_audio_on_pause = false; upload.sent_end = upload.speech_end + 128;
    process_reply_offer(); CHECK(accepts == 1);

    setup(0, false); change_offer_on_pause = true;
    process_reply_offer(); CHECK(accepts == 0 && stops == 0 && !paused && !observer_id);
    change_offer_on_pause = false;
    process_reply_offer(); CHECK(accepts == 1);

    setup(0, false); fail_accept = true;
    process_reply_offer(); CHECK(upload.running && current_state == APP_STATE_LISTENING && accepts == 0);
    setup(0, false); fail_pause = true;
    process_reply_offer(); CHECK(recoveries == 1 && accepts == 0);
    setup(0, false); failed_input_on_pause = true;
    process_reply_offer(); CHECK(recoveries == 1 && accepts == 0);
    setup(32, false); process_reply_offer();
    test_now += 10001;
    process_reply_offer(); CHECK(recoveries == 1 && cancels == 0 && accepts == 0);
    setup(32, false); conversation_active = false;
    process_reply_offer(); CHECK(!pauses && !stops && !cancels && !accepts);
    printf("PASS: %u production app handoff checks (backlog, fresh speech, pause race, stale offer, failure/timeout)\n", checks);
    return 0;
}
