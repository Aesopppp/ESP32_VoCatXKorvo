#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 每包仍是 20 ms 音频。绝对节拍吸收短调度抖动；补偿时至少间隔 10 ms，
 * 不把积压 PCM 一次性突发发送。超过 100 ms 的阻塞/暂停重新建立时基。
 * 此策略只安排发送时刻，不丢采样、不补零，也不触发打断。 */
#define AUDIO_UPLOAD_FRAME_US 20000
#define AUDIO_UPLOAD_MIN_INTERVAL_US 10000
#define AUDIO_UPLOAD_RESYNC_US 100000
typedef struct {
    int64_t next_us;
    int64_t last_us;
    bool started;
} audio_upload_clock_t;

static inline int64_t audio_upload_deadline(const audio_upload_clock_t *clock)
{
    if (!clock->started) return 0;
    int64_t earliest = clock->last_us + AUDIO_UPLOAD_MIN_INTERVAL_US;
    return clock->next_us > earliest ? clock->next_us : earliest;
}

static inline void audio_upload_advance(audio_upload_clock_t *clock, int64_t sent_us)
{
    if (!clock->started || sent_us - clock->next_us > AUDIO_UPLOAD_RESYNC_US)
        clock->next_us = sent_us;
    clock->next_us += AUDIO_UPLOAD_FRAME_US;
    clock->last_us = sent_us;
    clock->started = true;
}
