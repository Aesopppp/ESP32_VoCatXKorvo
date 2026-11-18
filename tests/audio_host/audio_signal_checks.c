/* 用生产解码入口/流生命周期验证幅度诊断，编码器与输出设备以桩替代。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/semphr.h"
#define AUDIO_OUT_SAMPLE_RATE 16000
#define AUDIO_OUT_STATE_PLAYING 2
static unsigned checks, summaries, writes;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define ESP_LOGI(tag,fmt,...) ((void)(tag), (void)(fmt), ++summaries)
static SemaphoreHandle_t output_lock=(void*)1;
static void *decoder=(void*)1;
static bool stream_open,stream_eof,stream_drained,output_started;
static size_t pcm_read,pcm_write,pcm_count;
static unsigned drain_blocks;
static uint32_t underruns,supplied_samples,decoded_packets,decode_max_us,write_max_us;
static uint32_t decode_total_us,decode_samples,decode_over_budget;
static uint32_t signal_peak,signal_samples,signal_nonzero;
static uint64_t signal_abs_sum;
static int64_t stats_at;
static int state,output_volume=60;
static int16_t decode_pcm[1920], source[960];
static int decoded_count=960;
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
int64_t esp_timer_get_time(void) { return 0; }
static int opus_dec_decode(void *d,const uint8_t *data,size_t len,int16_t *pcm,size_t cap)
{ (void)d; (void)data; (void)len; CHECK(cap>=sizeof(source)); memcpy(pcm,source,sizeof(source)); return decoded_count; }
static esp_err_t audio_out_write(const int16_t *pcm,size_t bytes)
{ (void)pcm; CHECK(bytes==(size_t)decoded_count*2); ++writes; return ESP_OK; }
#include "audio_signal.inc"
int main(void)
{
    uint8_t packet=1;
    audio_out_set_streaming(true);
    CHECK(audio_out_play_opus(&packet,1)==960);
    CHECK(signal_samples==960 && signal_peak==0 && signal_nonzero==0 && writes==1);
    audio_out_set_streaming(false); CHECK(summaries==1); /* 60 ms 短流也记录 EOF 摘要。 */
    audio_out_set_streaming(false); CHECK(summaries==1);
    stream_open=false; audio_out_set_streaming(true); CHECK(!signal_samples);
    source[0]=INT16_MIN; source[1]=INT16_MAX; source[2]=-1;
    CHECK(audio_out_play_opus(&packet,1)==960);
    CHECK(signal_peak==32768 && signal_abs_sum==65536 && signal_nonzero==3 && signal_samples==960);
    decoded_count=-1; CHECK(audio_out_play_opus(&packet,1)==-1);
    CHECK(signal_samples==960 && writes==2); /* 解码失败不计入有效样本。 */
    audio_out_set_streaming(false); CHECK(summaries==2);
    audio_out_set_streaming(true); CHECK(signal_samples==960 && !stream_eof); /* 同流续段不清累计值。 */
    printf("PASS: %u production PCM signal checks\n",checks); return 0;
}
