/* 直接运行生产 audio_in 的任务、编码分包和暂停恢复逻辑。
 * 用顺序 PCM 样本代替实际麦克风，用检查分包的编码器代替 Opus，不模拟声学。 */
#define BOARD_CONFIG_H
#define BSP_I2S_SAMPLE_RATE 16000
#define OPUS_SAMPLE_RATE 16000
#define OPUS_CHANNELS 1
#define OPUS_COMPLEXITY 2
#define ACOS_OPUS_BITRATE 24000
#define VAD_SILENCE_TIMEOUT_MS 800
#define VAD_MIN_UTTERANCE_MS 300
#define TASK_STACK_AUDIO_IN 4096
#include <stdio.h>
#include <stdlib.h>
#include "../../main/input_handoff.h"
#include "../../main/audio_in.c"

static unsigned checks, produced, encoded, packets, pause_count, finish_count, failed_events;
static unsigned fail_packet, stopped_while_paused;
static bool pause_partial;
static bool repeat_pause;
static audio_in_handle_t active_input;
static void (*task_entry)(void *);
static void *task_argument;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

struct test_sem { int value; bool mutex; };
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ SemaphoreHandle_t s = calloc(1, sizeof(*s)); s->value = 1; s->mutex = true; return s; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct test_sem)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{ (void)wait; if (!s->value) return pdFALSE; --s->value; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->value = 1; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
int xPortGetCoreID(void) { return 1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1024; }
unsigned esp_log_timestamp(void) { return produced / 16; }
int64_t esp_timer_get_time(void) { return (int64_t)produced * 1000 / 16; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *task, unsigned core)
{
    (void)name; (void)stack; (void)priority; (void)core;
    task_entry = fn; task_argument = arg; *task = arg; return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (!active_input->paused) return;
    ++pause_count;
    CHECK(active_input->paused_sem->value == 1); /* 上传已停在可恢复边界 */
    CHECK(active_input->pause_ack_epoch == active_input->pause_epoch);
    CHECK(finish_count == 0 && active_input->preroll.id == 3);
    CHECK(active_input->opus_enc != NULL);
    audio_in_upload_status_t status;
    audio_in_get_upload_status(active_input, &status);
    CHECK(status.sent_end == packets * 320);
    CHECK(input_handoff_decide(status.running, status.replaying,
          status.speech_end, status.sent_end, false, false) == INPUT_HANDOFF_DRAIN);
    if (stopped_while_paused) { active_input->running = false; return; }
    size_t partial = active_input->opus_samples;
    if (pause_partial && pause_count == 1) CHECK(partial == 192);
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(active_input->opus_samples == partial); /* 恢复不能清掉半包 */
    if (repeat_pause && pause_count == 1) {
        /* 主任务已恢复，但音频任务还没离开旧暂停循环，就再次要求暂停。 */
        ++active_input->pause_epoch;
        active_input->paused = true;
        active_input->paused_sem->value = 0;
    }
}

uint32_t ws_service_generation(void) { return 7; }
esp_err_t ws_service_send_opus_frame(const uint8_t *data, size_t len)
{
    (void)data; CHECK(len == 1); ++packets;
    if (packets == fail_packet) return ESP_ERR_TIMEOUT;
    if (packets == 18 || packets == 35) {
        ++active_input->pause_epoch;
        active_input->paused = true;
    }
    return ESP_OK;
}
BaseType_t event_bus_publish(event_type_t type, const void *data, size_t size)
{
    (void)size;
    if (type == EV_AUDIO_UPLOAD_FAILED) { ++failed_events; CHECK((uintptr_t)data == 7); }
    return pdTRUE;
}
opus_enc_handle_t opus_enc_create(const opus_enc_config_t *cfg)
{ CHECK(cfg->frame_ms == 20); return (opus_enc_handle_t)1; }
void opus_enc_destroy(opus_enc_handle_t encoder) { (void)encoder; }
int opus_enc_encode(opus_enc_handle_t encoder, const int16_t *pcm, int bytes,
                    uint8_t *output, int capacity)
{
    (void)encoder; (void)capacity; CHECK(bytes == 640);
    for (unsigned i = 0; i < 320; ++i) CHECK(pcm[i] == (int16_t)encoded++);
    output[0] = 1; return 1;
}
int wake_word_get_chunksize(void) { return 512; }
void wake_word_get_audio_progress(wake_word_audio_progress_t *p)
{ *p = (wake_word_audio_progress_t){29184, produced, 24000}; }
bool wake_word_get_preroll_info(wake_word_preroll_info_t *p)
{ *p = (wake_word_preroll_info_t){3, 56, 28672}; return true; }
size_t wake_word_finish_preroll(uint32_t id)
{ CHECK(id == 3); ++finish_count; return 0; }
size_t wake_word_read_audio(int16_t *pcm, size_t capacity, uint32_t *sample_end)
{
    CHECK(capacity == 1024);
    if (produced == 29184) { active_input->running = false; return 0; }
    /* 在第一帧后暂停，Opus 内还保存着 192 个样本。 */
    if (pause_partial && produced == 512) {
        ++active_input->pause_epoch;
        active_input->paused = true;
    }
    for (unsigned i = 0; i < 512; ++i) pcm[i] = (int16_t)produced++;
    *sample_end = produced;
    return 1024;
}

static void run_upload(unsigned failure, bool partial, bool stop)
{
    produced = encoded = packets = pause_count = finish_count = failed_events = 0;
    fail_packet = failure; pause_partial = partial; stopped_while_paused = stop;
    active_input = audio_in_create(NULL);
    CHECK(active_input != NULL);
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(active_input->replaying); /* 任务还未调度也已保护补传状态 */
    task_entry(task_argument);
    if (!failure && !stop) {
        CHECK(encoded == 29120 && packets == 91);
        CHECK(pause_count == (partial ? 3 : 2) + (repeat_pause ? 1 : 0));
        CHECK(active_input->preroll.id == 0 && !active_input->replaying);
        CHECK(active_input->sent_end == encoded && finish_count == 1);
    } else {
        CHECK(finish_count == 1 && !active_input->replaying);
        CHECK(failed_events == (failure ? 1 : 0));
        if (failure) CHECK(active_input->sent_end == (packets - 1) * 320);
    }
    audio_in_destroy(active_input);
}
int main(void)
{
    run_upload(0, false, false);
    run_upload(0, true, false);
    repeat_pause = true;
    run_upload(0, false, false);
    repeat_pause = false;
    run_upload(12, false, false);
    run_upload(0, false, true);
    printf("PASS: %u upload checks (production PCM/Opus packetization, pause/resume, failure/stop)\n", checks);
    return 0;
}
