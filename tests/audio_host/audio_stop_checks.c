/* 运行生产 PCM 写入/停止函数；验证网络信号解除背压、清缓存且刷新 DMA。
 * 驱动写入在主机记录为块，不模拟真实 I2S 时序或多核调度。 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "../../main/pcm_policy.h"
#define AUDIO_OUT_STATE_IDLE 1
#define AUDIO_OUT_STATE_ERROR 3
static void *speaker=(void*)1;
static SemaphoreHandle_t output_lock=(void*)2;
static bool stop_requested, playback_streaming, stream_open, stream_eof, output_started, stream_drained;
static int state;
static int16_t pcm_ring[PCM_CAPACITY], last_sample;
static size_t pcm_read, pcm_write, pcm_count;
static unsigned checks, blocks, waits;
static int64_t time_us;
static bool interrupt_on_wait;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks) { (void)s; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
int64_t esp_timer_get_time(void) { return time_us; }
void audio_out_request_stop(void);
void vTaskDelay(TickType_t ticks)
{ time_us+=(int64_t)ticks*1000; ++waits; if(interrupt_on_wait) audio_out_request_stop(); }
static esp_err_t write_block(int16_t *pcm)
{
    ++blocks;
    if(blocks==1) { CHECK(pcm[0]<1000 && pcm[PCM_BLOCK-1]==0); }
    else for(unsigned i=0;i<PCM_BLOCK;++i) CHECK(pcm[i]==0);
    return ESP_OK;
}
#include "audio_stop.inc"
int main(void)
{
    stream_open=output_started=playback_streaming=true; pcm_count=PCM_CAPACITY;
    pcm_write=123; pcm_read=23; last_sample=1000;
    int16_t pcm[320]={1}; interrupt_on_wait=true;
    CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_ERR_INVALID_STATE);
    CHECK(waits==1 && audio_out_stop_requested()); /* 不等 2 秒写超时。 */
    CHECK(audio_out_stop()==ESP_OK);
    CHECK(!pcm_count && !pcm_read && !pcm_write && !stream_open && !output_started);
    CHECK(!playback_streaming && stream_drained && !audio_out_stop_requested());
    CHECK(blocks==PCM_DMA_BLOCKS+2 && last_sample==0 && speaker==(void*)1);
    stream_open=true; CHECK(audio_out_write(pcm,sizeof(pcm))==ESP_OK && pcm_count==320);
    CHECK(pcm_ring[0]==1); /* 新流不会继承旧队列。 */
    printf("PASS: %u production PCM stop checks\n",checks); return 0;
}
