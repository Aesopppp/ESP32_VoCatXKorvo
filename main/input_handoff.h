#ifndef INPUT_HANDOFF_H
#define INPUT_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>
#include "barge_in_guard.h"

/* 输入交接使用采样位置而不是任务耗时。FIFO 最多约 10 秒，位置差远小于
 * INT32_MAX；无符号差转有符号可跨过 32 位采样计数回绕。 */
static inline bool input_samples_pending(uint32_t speech_end, uint32_t sent_end)
{
    return (int32_t)(speech_end - sent_end) > 0;
}

typedef enum {
    INPUT_HANDOFF_ACCEPT,
    INPUT_HANDOFF_DRAIN,
    INPUT_HANDOFF_CANCEL,
} input_handoff_action_t;

/* 仅在收到本轮回答候选之后观察新采集的 AFE 帧。旧 FIFO 的上传进度
 * 不参与此检测，不能用“还有 32 ms 未上传”代替“用户重新讲话”。 */
typedef struct {
    barge_in_guard_t guard;
    bool confirmed;
} input_handoff_speech_t;

static inline void input_handoff_speech_reset(input_handoff_speech_t *speech)
{
    barge_in_guard_reset(&speech->guard);
    speech->confirmed = false;
}

static inline void input_handoff_speech_feed(input_handoff_speech_t *speech,
    bool vad, int32_t energy, uint32_t samples)
{
    if (!speech->confirmed)
        speech->confirmed = barge_in_guard_feed(&speech->guard, vad, energy, samples,
                                                BARGE_IN_GUARD_MS) == BARGE_IN_CONFIRMED;
}

/* 积压只等待排空；新采集的人声才可能取消。候选不足 480 ms 时先等待判定，
 * 不让一帧人声或短时噪声直接触发取消。 */
static inline input_handoff_action_t input_handoff_decide(bool running,
    bool replaying, uint32_t speech_end, uint32_t sent_end,
    bool speech_pending, bool speech_confirmed)
{
    if (!running) return INPUT_HANDOFF_ACCEPT;
    if (speech_confirmed) return INPUT_HANDOFF_CANCEL;
    if (replaying || speech_pending || input_samples_pending(speech_end, sent_end))
        return INPUT_HANDOFF_DRAIN;
    return INPUT_HANDOFF_ACCEPT;
}

#endif
