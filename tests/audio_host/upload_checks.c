/* 直接运行生产 audio_in 的任务、编码分包和暂停恢复逻辑。
 * 用顺序 PCM 样本代替实际麦克风，用检查分包的编码器代替 Opus，不模拟声学。 */
#include <stdio.h>
#include <stdlib.h>
#include "../../main/audio_in.c"

static unsigned checks, produced, encoded, packets, pause_count, failed_events;
static unsigned fail_packet, stopped_while_paused;
static bool pause_partial;
static bool repeat_pause;
static int64_t test_time_us;
static unsigned encode_cost_us, send_cost_us, scheduler_late_us;
static unsigned stall_packet, stall_us, max_backlog;
static unsigned speech_starts, speech_ends, utterances, utterance_allocations;
static bool allocation_fails;
static bool wifi_audio_active, wifi_mode_failure, task_create_fails;
static unsigned wifi_mode_calls;
static audio_in_handle_t active_input;
static void (*task_entry)(void *);
static void *task_argument;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
void *test_heap_caps_malloc(size_t bytes, unsigned caps)
{
    ++utterance_allocations; CHECK(caps & MALLOC_CAP_SPIRAM);
    return allocation_fails ? NULL : malloc(bytes);
}

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
unsigned esp_log_timestamp(void) { return (unsigned)(test_time_us / 1000); }
int64_t esp_timer_get_time(void) { return test_time_us; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *task, unsigned core)
{
    (void)name; (void)stack; (void)priority; (void)core;
    CHECK(wifi_audio_active);
    if (task_create_fails) return pdFALSE;
    task_entry = fn; task_argument = arg; *task = arg; return pdPASS;
}
esp_err_t wifi_service_set_audio_active(bool active)
{
    wifi_audio_active = active; ++wifi_mode_calls;
    return wifi_mode_failure ? ESP_FAIL : ESP_OK;
}
void vTaskDelay(TickType_t ticks)
{
    test_time_us += (int64_t)ticks * 1000 + scheduler_late_us;
    if (!active_input->paused) return;
    /* 帧间主动让出 CPU 不等于暂停确认；下一次生产 wait 才会确认本次 epoch。 */
    if (active_input->pause_ack_epoch != active_input->pause_epoch) return;
    ++pause_count;
    CHECK(active_input->paused_sem->value == 1); /* 上传已停在可恢复边界 */
    CHECK(active_input->pause_ack_epoch == active_input->pause_epoch);
    CHECK(active_input->opus_enc != NULL);
    audio_in_upload_status_t status;
    audio_in_get_upload_status(active_input, &status);
    CHECK(status.sent_end == packets * 320);
    CHECK(status.running);
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
    (void)data; CHECK(len == 1);
    ++packets;
    test_time_us += send_cost_us;
    if (packets == stall_packet) test_time_us += stall_us;
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
    if (type == EV_AUDIO_SPEECH_START) ++speech_starts;
    if (type == EV_AUDIO_SPEECH_END) ++speech_ends;
    return pdTRUE;
}
opus_enc_handle_t opus_enc_create(const opus_enc_config_t *cfg)
{
    CHECK(cfg->frame_ms == 20 && cfg->sample_rate == 16000);
    CHECK(cfg->channels == 1 && cfg->bitrate == 24000 && cfg->complexity == 0);
    return (opus_enc_handle_t)1;
}
void opus_enc_destroy(opus_enc_handle_t encoder) { (void)encoder; }
int opus_enc_encode(opus_enc_handle_t encoder, const int16_t *pcm, int bytes,
                    uint8_t *output, int capacity)
{
    (void)encoder; (void)capacity; CHECK(bytes == 640);
    for (unsigned i = 0; i < 320; ++i) CHECK(pcm[i] == (int16_t)encoded++);
    test_time_us += encode_cost_us;
    output[0] = 1; return 1;
}
int wake_word_get_chunksize(void) { return 512; }
void wake_word_get_audio_progress(wake_word_audio_progress_t *p)
{ *p = (wake_word_audio_progress_t){29184, produced, 24000}; }
size_t wake_word_read_audio(int16_t *pcm, size_t capacity, uint32_t *sample_end)
{
    CHECK(capacity == 1024);
    if (produced == 29184) { active_input->running = false; return 0; }
    /* AFE 每 32 ms 产出 512 样本；尚未到达的采样不可凭空读取。
     * 模拟网络阻塞时继续采样，以验证恢复后真实数据被有序追赶。 */
    unsigned available = (unsigned)(test_time_us / 32000) * 512;
    if (available > 29184) available = 29184;
    unsigned backlog = available - produced;
    if (backlog > max_backlog) max_backlog = backlog;
    if (backlog < 512) return 0;
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
    produced = encoded = packets = pause_count = failed_events = 0;
    test_time_us = 0; max_backlog = 0;
    fail_packet = failure; pause_partial = partial; stopped_while_paused = stop;
    active_input = audio_in_create(NULL);
    CHECK(active_input != NULL);
    CHECK(active_input->vad_buf == NULL && active_input->vad_buf_cap == 0);
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(wifi_audio_active);
    task_entry(task_argument);
    if (!failure && !stop) {
        CHECK(encoded == 29120 && packets == 91);
        CHECK(pause_count == (partial ? 3 : 2) + (repeat_pause ? 1 : 0));
        CHECK(active_input->sent_end == encoded);
        /* 约 1.82 秒真实音频。中段 135 ms 网络阻塞应在结束前追上；
         * 不能以重复采样/跳过采样/虚构静音通过（编码器逐样本检查）。 */
        CHECK(test_time_us >= 1824000 && test_time_us < 1974000);
        CHECK(max_backlog <= 8192); /* 不能超出生产 512 ms FIFO 的容量。 */
        CHECK(active_input->vad_buf == NULL);
    } else {
        CHECK(failed_events == (failure ? 1 : 0));
        if (failure) CHECK(active_input->sent_end == (packets - 1) * 320);
    }
    /* 这些场景短于 5 秒，统计未清零；累计值必须包含失败发送和网络阻塞。 */
    CHECK(active_input->encode_work_us == (uint64_t)packets * encode_cost_us);
    CHECK(active_input->send_work_us == (uint64_t)packets * send_cost_us +
          ((stall_packet && packets >= stall_packet) ? stall_us : 0));
    audio_in_destroy(active_input);
    CHECK(!wifi_audio_active);
}
static void check_wifi_mode_lifecycle(void)
{
    active_input = audio_in_create(NULL);
    task_create_fails = true;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_ERR_NO_MEM);
    CHECK(!wifi_audio_active && !active_input->running && !active_input->task_h);
    task_create_fails = false;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    unsigned calls = wifi_mode_calls;
    CHECK(audio_in_start_from_ringbuf(active_input, (void *)1) == ESP_OK);
    CHECK(wifi_mode_calls == calls); /* 已运行/暂停恢复时不重复切换。 */
    CHECK(audio_in_stop(active_input) == ESP_ERR_TIMEOUT);
    CHECK(wifi_audio_active); /* 任务未退出，不能提前还原 Wi-Fi 模式。 */
    task_entry(task_argument); /* 已请求停止，执行退出清理并确认。 */
    CHECK(audio_in_stop(active_input) == ESP_OK && !wifi_audio_active);
    audio_in_destroy(active_input);
}
static void utterance_callback(void *arg, const int16_t *pcm, size_t bytes)
{
    CHECK(arg == active_input); CHECK(pcm == active_input->vad_buf);
    CHECK(bytes > 0 && bytes <= active_input->vad_buf_cap); ++utterances;
}
static void feed_utterance(void)
{
    int16_t pcm[512];
    unsigned started = speech_starts, ended = speech_ends;
    for (unsigned f = 0; f < 100; ++f) {
        for (unsigned i = 0; i < 512; ++i) pcm[i] = f < 60 ? 32767 : 0;
        process_vad_frame(active_input, pcm, 512, f * 32);
        CHECK(active_input->energy_sum >= 0); /* 饱和采样的 8 帧均方累加不溢出。 */
    }
    CHECK(speech_starts == started + 1 && speech_ends == ended + 1);
    CHECK(active_input->vad_state == VAD_STATE_IDLE);
}
static void check_optional_utterance(void)
{
    active_input = audio_in_create(NULL);
    CHECK(active_input && !active_input->vad_buf && !utterance_allocations);
    feed_utterance(); CHECK(!utterances && !utterance_allocations);
    allocation_fails = true;
    CHECK(audio_in_register_utterance_cb(active_input, utterance_callback, active_input) == ESP_ERR_NO_MEM);
    CHECK(!active_input->utt_cb && !active_input->vad_buf);
    allocation_fails = false;
    CHECK(audio_in_register_utterance_cb(active_input, utterance_callback, active_input) == ESP_OK);
    CHECK(active_input->vad_buf_cap == 640000);
    feed_utterance(); CHECK(utterances == 1 && utterance_allocations == 2);
    active_input->running = true;
    CHECK(audio_in_register_utterance_cb(active_input, NULL, NULL) == ESP_ERR_INVALID_STATE);
    active_input->running = false;
    CHECK(audio_in_register_utterance_cb(active_input, NULL, NULL) == ESP_OK);
    CHECK(!active_input->vad_buf && !active_input->vad_buf_cap);
    feed_utterance(); CHECK(utterances == 1);
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
    encode_cost_us=6000; send_cost_us=2000; scheduler_late_us=3000;
    run_upload(0, false, false);
    run_upload(0, true, false);
    stall_packet=45; stall_us=135000;
    run_upload(0, false, false);
    check_optional_utterance();
    check_wifi_mode_lifecycle();
    wifi_mode_failure = true;
    run_upload(0, false, false); /* 省电设置失败不阻断上传，不发布上传失败事件。 */
    printf("PASS: %u upload checks (sample clock, network stalls, partial frames, optional VAD, Wi-Fi lifecycle, timing totals)\n", checks);
    return 0;
}
