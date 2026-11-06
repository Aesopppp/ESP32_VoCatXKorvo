/* 重放真实滞后值及多轮交接，使用生产交接/人声确认/回复流，不能靠包积压取消。 */
#include "../main/input_handoff.h"
#include "../main/response_stream.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

static input_handoff_action_t decide(const input_handoff_speech_t *speech,
                                     bool replaying, uint32_t end, uint32_t sent)
{
    return input_handoff_decide(true, replaying, end, sent,
                                speech->guard.samples != 0, speech->confirmed);
}

int main(void)
{
    const unsigned lag_ms[] = {32, 44, 1040, 1400};
    input_handoff_speech_t speech = {0};
    response_stream_t response;
    unsigned accepted = 0, cancelled = 0;
    for (unsigned turn = 1; turn <= 4; ++turn) {
        input_handoff_speech_reset(&speech);
        response_stream_begin(&response, turn);
        response_stream_hold_input(&response);
        uint32_t end = 48000, sent = end - lag_ms[turn - 1] * 16;
        while (input_samples_pending(end, sent)) {
            input_handoff_action_t action = decide(&speech, false, end, sent);
            if (action == INPUT_HANDOFF_CANCEL) ++cancelled;
            CHECK(action == INPUT_HANDOFF_DRAIN);
            CHECK(!response_stream_accept_audio(&response, turn));
            /* 旧 PCM 可以大量补传，但不能回送进“新讲话”观察器。 */
            sent += 320;
        }
        CHECK(decide(&speech, false, end, sent) == INPUT_HANDOFF_ACCEPT);
        CHECK(response_stream_release_input(&response, turn));
        CHECK(response_stream_accept_audio(&response, turn));
        proto_msg_t done = {.evt = PROTO_EVT_RESPONSE_DONE};
        CHECK(response_stream_control(&response, turn, &done) == RESPONSE_STREAM_AUDIO_END);
        CHECK(response_stream_finish_if_drained(&response, true, true, true));
        ++accepted;
    }
    CHECK(accepted == 4 && cancelled == 0);

    /* 1792 ms 句首补传：上传末端未覆盖人声、或补传保护仍在，都不能停输入。 */
    CHECK(decide(&speech, true, 28672, 18 * 320) == INPUT_HANDOFF_DRAIN);
    CHECK(decide(&speech, true, 28672, 28800) == INPUT_HANDOFF_DRAIN);
    CHECK(decide(&speech, false, 28672, 28800) == INPUT_HANDOFF_ACCEPT);

    /* 一帧人声/尖峰只等待，不取消；过长间隙会清掉候选。 */
    input_handoff_speech_feed(&speech, true, 20000, 512);
    CHECK(decide(&speech, false, 48000, 48000) == INPUT_HANDOFF_DRAIN);
    for (unsigned i = 0; i < 4; ++i) input_handoff_speech_feed(&speech, false, 0, 512);
    CHECK(decide(&speech, false, 48000, 48000) == INPUT_HANDOFF_ACCEPT);
    for (unsigned i = 0; i < 40; ++i) {
        input_handoff_speech_feed(&speech, false, 100000, 512);
        input_handoff_speech_feed(&speech, true, BARGE_IN_MIN_ENERGY - 1, 512);
        CHECK(!speech.confirmed && !speech.guard.samples);
    }

    /* 只对 offer 之后新到的 480 ms 强人声取消；不以“积压超过某个阈值”代替。 */
    for (unsigned i = 0; i < 14; ++i) {
        input_handoff_speech_feed(&speech, true, 20000, 512);
        CHECK(decide(&speech, false, 48000, 48000) == INPUT_HANDOFF_DRAIN);
    }
    input_handoff_speech_feed(&speech, true, 20000, 512);
    CHECK(decide(&speech, false, 48000, 48000) == INPUT_HANDOFF_CANCEL);
    /* 主任务有调度延迟时确认不能丢，新的 offer/重连则必须复位。 */
    input_handoff_speech_feed(&speech, false, 0, 512);
    CHECK(speech.confirmed);
    input_handoff_speech_reset(&speech);
    CHECK(decide(&speech, false, 48000, 48000) == INPUT_HANDOFF_ACCEPT);
    for (unsigned i = 0; i < 8; ++i) input_handoff_speech_feed(&speech, true, 20000, 512);
    for (unsigned i = 0; i < 3; ++i) input_handoff_speech_feed(&speech, false, 0, 512);
    for (unsigned i = 0; i < 7; ++i) input_handoff_speech_feed(&speech, true, 20000, 512);
    CHECK(speech.confirmed); /* 原有短隙容忍保留 */

    /* 暂停前已排空，但暂停后又入一帧：恢复上传，不误取消/丢半包。 */
    input_handoff_speech_reset(&speech);
    CHECK(decide(&speech, false, 16000, 16000) == INPUT_HANDOFF_ACCEPT);
    CHECK(decide(&speech, false, 16512, 16000) == INPUT_HANDOFF_DRAIN);
    CHECK(decide(&speech, false, 16512, 16640) == INPUT_HANDOFF_ACCEPT);
    printf("PASS: %u handoff regression checks; four logged backlogs drain, four replies play, zero cancellations\n", checks);
    return 0;
}
