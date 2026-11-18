/* 运行生产 AFE->FIFO 门禁。环形缓冲用固定帧计数桩替代，不模拟声学和并发。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/wake_word.h"
#include "freertos/semphr.h"
#include "esp_log.h"
static unsigned checks, queued, sent, returned;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define AUDIO_UPLOAD_FIFO_MS 512u
#define AUDIO_DIAGNOSTIC_ENERGY 2500
#define BSP_I2S_SAMPLE_RATE 16000
#define VAD_SPEECH 1
typedef enum { APP_STATE_WAIT_WAKE, APP_STATE_LISTENING, APP_STATE_PLAYING } app_state_t;
typedef struct { int16_t *data; int data_size, vad_state; } afe_fetch_result_t;
static app_state_t current;
static app_state_t app_state_get(void) { return current; }
static RingbufHandle_t s_ringbuf=(void*)1;
static SemaphoreHandle_t s_audio_buffer_lock=(void*)2;
static bool s_audio_buffer_enabled=true;
static int s_fetch_chunksize=512;
static wake_word_audio_progress_t s_audio_progress;
static uint32_t s_overflow_log_ms, s_overflow_frames;
static int16_t frame[512];
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static void vRingbufferGetInfo(void *r,void *a,void *b,void *c,void *d,UBaseType_t *n)
{ (void)r; (void)a; (void)b; (void)c; (void)d; *n=queued; }
static void *xRingbufferReceive(void *r,size_t *bytes,int t)
{ (void)r; (void)t; if(!queued) return NULL; --queued; *bytes=sizeof(frame); return frame; }
static void vRingbufferReturnItem(void *r,void *p) { (void)r; (void)p; ++returned; }
static int xRingbufferSend(void *r,void *p,size_t bytes,int t)
{ (void)r; (void)p; (void)t; CHECK(bytes==sizeof(frame)); ++queued; ++sent; return pdTRUE; }
unsigned esp_log_timestamp(void) { return 0; }
#include "wake_fifo.inc"
int main(void)
{
    for(unsigned i=0;i<512;++i) frame[i]=1000;
    afe_fetch_result_t res={frame,sizeof(frame),VAD_SPEECH};
    current=APP_STATE_WAIT_WAKE; buffer_afe_audio(&res); CHECK(!queued);
    current=APP_STATE_LISTENING; buffer_afe_audio(&res); CHECK(queued==1 && sent==1);
    current=APP_STATE_PLAYING; buffer_afe_audio(&res); CHECK(queued==2 && sent==2);
    CHECK(s_audio_progress.written_end==1024 && s_audio_progress.speech_end==1024);
    wake_word_set_audio_buffer_enabled(false);
    CHECK(!queued && returned==2 && !s_audio_progress.written_end);
    for(unsigned i=0;i<10000;++i) buffer_afe_audio(&res);
    CHECK(!queued && sent==2 && !s_overflow_frames && !s_audio_progress.written_end);
    wake_word_set_audio_buffer_enabled(true); current=APP_STATE_WAIT_WAKE;
    buffer_afe_audio(&res); CHECK(!queued);
    current=APP_STATE_LISTENING; buffer_afe_audio(&res); CHECK(queued==1);
    current=APP_STATE_PLAYING;
    for(unsigned i=0;i<30;++i) buffer_afe_audio(&res);
    CHECK(queued==16 && s_overflow_frames==15); /* 普通全双工仍保留原有 512 ms 上限。 */
    CHECK(s_audio_progress.written_end==31*512 && s_audio_progress.read_end==15*512);
    printf("PASS: %u production microphone FIFO checks\n",checks); return 0;
}
