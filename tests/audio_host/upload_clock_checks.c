/* 运行生产节拍策略：短抖动不积累、长期阻塞不突发、长时间无时基溢出。 */
#include <stdio.h>
#include <stdlib.h>
#include "../../main/audio_upload_policy.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
    audio_upload_clock_t clock = {0};
    int64_t now = 6000000000LL, origin = now;
    CHECK(audio_upload_deadline(&clock) == 0);
    audio_upload_advance(&clock, now);
    for (unsigned i=1; i<10000; ++i) {
        now += 6000; /* 编码计算 */
        int64_t deadline=audio_upload_deadline(&clock);
        if (now<deadline) now=deadline+3000; /* 每次唤醒晚 3 ms */
        CHECK(now-clock.last_us >= AUDIO_UPLOAD_MIN_INTERVAL_US);
        audio_upload_advance(&clock, now);
        CHECK(now-origin <= (int64_t)i*AUDIO_UPLOAD_FRAME_US+3000);
    }
    now=clock.next_us+80000; /* 短阻塞允许逐包有限补偿 */
    audio_upload_advance(&clock, now);
    CHECK(audio_upload_deadline(&clock)==now+AUDIO_UPLOAD_MIN_INTERVAL_US);
    now=clock.next_us+AUDIO_UPLOAD_RESYNC_US+1;
    audio_upload_advance(&clock, now);
    CHECK(audio_upload_deadline(&clock)==now+AUDIO_UPLOAD_FRAME_US);
    printf("PASS: %u upload clock checks\n",checks);
    return 0;
}
