#ifndef RESPONSE_FLOW_H
#define RESPONSE_FLOW_H

#include <stdbool.h>

/* 回复窗口由平台事件推进。speech_started 直接丢弃旧输出，不进入取消确认状态。 */
typedef enum {
    RESPONSE_IDLE, RESPONSE_WAITING, RESPONSE_PLAYING,
    RESPONSE_DISCARD
} response_phase_t;
typedef enum { RESPONSE_END_IGNORE, RESPONSE_END_NORMAL } response_end_t;

/* A wire end boundary alone cannot complete a buffered reply. Keeping the
 * phase active during drain also permits barge-in on a locally queued tail. */
static inline bool response_playback_finished(response_phase_t phase, bool eof, bool drained)
{
    return eof && drained && (phase == RESPONSE_PLAYING || phase == RESPONSE_WAITING);
}

static inline void response_expect(response_phase_t *phase)
{
    if (*phase == RESPONSE_IDLE || *phase == RESPONSE_DISCARD)
        *phase = RESPONSE_WAITING;
}
static inline bool response_accept_audio(response_phase_t *phase)
{
    if (*phase != RESPONSE_WAITING && *phase != RESPONSE_PLAYING) return false;
    *phase = RESPONSE_PLAYING;
    return true;
}
static inline response_end_t response_finish(response_phase_t *phase)
{
    if (*phase == RESPONSE_WAITING || *phase == RESPONSE_PLAYING) {
        *phase = RESPONSE_IDLE;
        return RESPONSE_END_NORMAL;
    }
    return RESPONSE_END_IGNORE;
}
#endif
