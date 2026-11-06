/**
 * @file app_main.c
 * @brief YMOS VoCat 入口 — FreeRTOS 任务编排与事件分发中枢
 *
 * ══════════════════════════════════════════════════════════════════════════════
 * 系统架构
 * ══════════════════════════════════════════════════════════════════════════════
 * P0 收音链路：MIC(ES7210) → I²S DMA → AFE(WakeNet VAD) → RingBuffer
 *              → audio_in(VAD切句) → Opus编码 → WS JSON帧 → ACOS云端
 *
 * P1放音链路：ACOS WS(Opus) → Opus解码 → ES8311 DAC → 功放 → 扬声器
 *              支持 Barge-In 打断
 *
 * 依赖：ESP-IDF v6.1 + esp-sr
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "board_service.h"
#include "event_bus.h"
#include "app_state.h"
#include "wifi_service.h"
#include "cfg_service.h"
#include "log_service.h"
#include "led_service.h"
#include "audio_out.h"
#include "ws_service.h"
#include "proto_service.h"
#include "audio_in.h"
#include "input_handoff.h"
#include "wake_word.h"
#include "provisioning_service.h"
#include "button_service.h"
#include "button_policy.h"
#include "ui_service.h"
#include "ui_policy.h"
#include "display_service.h"

static const char *TAG = "app_main";

/* ── 内部任务 & 队列 ────────────────────────────────────────── */

static event_bus_t s_bus;
static led_service_t s_led;
static int s_saved_volume;
static int64_t s_volume_save_at_ms;
static int s_brightness, s_saved_brightness;
static int64_t s_brightness_save_at_ms;
static bool s_ui_ready;


#define MAIN_TASK_PRIORITY   5
#define MAIN_TASK_STACK     8192

/* ── 系统初始化 ─────────────────────────────────────────────── */

static esp_err_t system_init(void)
{
    ESP_LOGI(TAG, "=== YMOS VoCat v1.2 starting (ESP-IDF v6.1, touchscreen, no MQTT) ===");

#if CONFIG_SPIRAM
    if (esp_psram_get_size() > 0) {
        ESP_LOGI(TAG, "PSRAM size: %zu MB", esp_psram_get_size() / 1024 / 1024);
    }
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(cfg_service_init());
    ESP_ERROR_CHECK(app_state_init(s_bus));
    /* wifi_service_init owns netif and default event loop creation. */
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_LOGI(TAG, "Wi-Fi service ready");

    ESP_ERROR_CHECK(board_service_init());
    s_led = led_service_init();
    if (s_led == NULL) {
        ESP_LOGE(TAG, "LED service init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(audio_out_init(ES8311_I2C_ADDR));
    int volume = cfg_service_get_int(CFG_KEY_VOLUME, AUDIO_OUT_DEFAULT_VOLUME);
    if (volume < 0 || volume > 100) volume = AUDIO_OUT_DEFAULT_VOLUME;
    ESP_ERROR_CHECK(audio_out_set_volume(volume));
    s_saved_volume = volume;
    ESP_LOGI(TAG, "Audio output ready (volume=%d)", volume);


    ESP_ERROR_CHECK(ws_service_init());
    ESP_ERROR_CHECK(provisioning_service_init(s_bus));
    audio_in_config_t input_config = audio_in_default_config();
    input_config.bus = s_bus;
    if (!audio_in_create(&input_config)) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(wake_word_init());
    ESP_LOGI(TAG, "WebSocket service ready");
    esp_err_t button_err = button_service_init();
    if (button_err != ESP_OK) {
        ESP_LOGW(TAG, "Buttons unavailable: %s", esp_err_to_name(button_err));
    }

    /* 音频先取得所需资源；UI 失败降级为语音和 BOOT，不反复重启设备。 */
    s_brightness = ui_brightness_restore(cfg_service_get_int(CFG_KEY_BRIGHTNESS, UI_BRIGHTNESS_DEFAULT));
    s_saved_brightness = s_brightness;
    esp_err_t ui_err = ui_service_init();
    s_ui_ready = ui_err == ESP_OK;
    if (s_ui_ready) {
        ui_err = display_service_set_brightness(s_brightness);
        if (ui_err != ESP_OK) ESP_LOGW(TAG, "Backlight failed: %s", esp_err_to_name(ui_err));
    } else ESP_LOGW(TAG, "Display unavailable: %s; voice and BOOT remain active", esp_err_to_name(ui_err));

    return ESP_OK;
}

/* ── 事件分发 ──────────────────────────────────────────────── */

/* Only this task changes session state or starts/stops the WS client. */
static bool conversation_active;
/* 主任务拥有。结束对话/进入配网先清除此意图，迟到的错误不得重新拉起会话。 */
static bool session_requested;
static int64_t retry_at_ms, auth_deadline_ms, cancel_deadline_ms, response_deadline_ms;
static int64_t resume_deadline_ms;
static int64_t cancel_probe_ms;
static struct {
    ws_reply_offer_t offer;
    uint32_t observer;
    int64_t started_ms;
    bool logged_wait;
} input_handoff;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void reset_input_handoff(void)
{
    wake_word_end_input_handoff(input_handoff.observer);
    memset(&input_handoff, 0, sizeof(input_handoff));
}

static void end_conversation(const char *reason)
{
    reset_input_handoff();
    session_requested = conversation_active = false;
    retry_at_ms = auth_deadline_ms = cancel_deadline_ms = response_deadline_ms = 0;
    resume_deadline_ms = cancel_probe_ms = 0;
    /* 先停上传/播放，再启用 WakeNet，避免结束时把自己的尾音当成新唤醒。 */
    app_state_pause_silence_timer();
    esp_err_t input_err = audio_in_stop(audio_in_get_handle());
    if (input_err != ESP_OK) ESP_LOGW(TAG, "Audio stop failed: %s", esp_err_to_name(input_err));
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop failed: %s", esp_err_to_name(err));
    app_state_back_to_wait_wake(false);
    wake_word_reset_barge_in();
    wake_word_clear_audio_buffer();
    if (wifi_service_get_state() == WIFI_STATE_GOT_IP) led_set_waiting_wake();
    else led_set_wifi_err();
    ESP_LOGI(TAG, "Conversation ended: %s; waiting for wake word", reason);
}

static void recover_session(const char *reason)
{
    if (!session_requested || provisioning_service_is_active()) return;
    reset_input_handoff();
    ESP_LOGW(TAG, "Session recovery: %s", reason);
    if (conversation_active && !resume_deadline_ms) resume_deadline_ms = now_ms() + 60000;
    app_state_back_to_wait_wake(false);
    wake_word_reset_barge_in();
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop pending: %s", esp_err_to_name(err));
    wake_word_clear_audio_buffer();
    auth_deadline_ms = cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    retry_at_ms = now_ms() + 5000;
    ESP_LOGW(TAG, "Reconnect scheduled in 5000 ms; resume_conversation=%d (no voice required)",
             conversation_active);
    led_set_ws_err();
}

static void resume_listening(bool keep_buffered_speech, const char *reason)
{
    reset_input_handoff();
    /* 切模式会清空 FIFO/采样位置，必须在启动上传任务之前完成，不能让新的
     * sent_end 和被复位的 AFE 位置属于不同基准。 */
    if (wake_word_get_mode() != WAKE_WORD_MODE_LITE) {
        if (wake_word_set_mode(WAKE_WORD_MODE_LITE) != ESP_OK) {
            recover_session("could not select listening AFE");
            return;
        }
        ESP_LOGI(TAG, "AFE mode -> LITE (WakeNet disabled for audio)");
    }
    if (!keep_buffered_speech) wake_word_clear_audio_buffer();
    cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    if (app_state_start_listening() != ESP_OK) {
        recover_session("could not start audio upload");
        return;
    }
    wake_word_reset_barge_in();

    resume_deadline_ms = 0;
    led_set_listening();
    ESP_LOGI(TAG, "Conversation listening; source=%s; ready for the next question", reason);
}

/* 唤醒词、按键和触屏共用入口：待机不上传，认证成功后才开始监听。 */
static void start_conversation(const char *reason)
{
    if (provisioning_service_is_active()) {
        wake_word_reset();
        ESP_LOGI(TAG, "Talk ignored during Wi-Fi setup");
        return;
    }
    if (wifi_service_get_state() != WIFI_STATE_GOT_IP) {
        wake_word_reset();
        led_set_wifi_err();
        ESP_LOGW(TAG, "Talk ignored: Wi-Fi not ready");
        return;
    }
    if (session_requested) {
        if (app_state_get() == APP_STATE_LISTENING) app_state_reset_silence_timer();
        return; /* 连接中/取消中重复按键不重复发送请求。 */
    }
    session_requested = true;
    wake_word_set_mode(WAKE_WORD_MODE_LITE);
    if (ws_service_get_state() == WS_STATE_CONNECTED) {
        conversation_active = true;
        resume_listening(false, reason);
    } else {
        app_state_set(APP_STATE_WS_CONNECTING_WAKE);
        retry_at_ms = now_ms();
        led_set_connecting();
        ESP_LOGI(TAG, "Conversation requested: %s; opening WebSocket", reason);
    }
}

/* 手动/语音打断共用取消边界；只在主任务操作 WS，保留已有预录音补传逻辑。 */
static void interrupt_response(bool manual)
{
    const bool unfinished_input = app_state_get() == APP_STATE_LISTENING;
    if (!conversation_active || (!unfinished_input && app_state_get() != APP_STATE_PLAYING) ||
        ws_service_get_state() != WS_STATE_CONNECTED) return;
    if (manual) wake_word_protect_preroll();
    esp_err_t err = ws_service_cancel_response();
    if (err == ESP_ERR_NOT_FOUND) {
        /* 回答恰好结束，队列内的正常完成事件负责恢复监听。 */
        wake_word_reset_barge_in();
        if (unfinished_input)
            audio_in_start_from_ringbuf(audio_in_get_handle(), wake_word_get_ringbuf());
        return;
    }
    if (err != ESP_OK) { recover_session("response.cancel send failed"); return; }
    app_state_set(APP_STATE_CANCELLING);
    app_state_pause_silence_timer();
    response_deadline_ms = 0;
    cancel_probe_ms = now_ms() + 3000;
    cancel_deadline_ms = now_ms() + 8000;
    ESP_LOGI(TAG, "Playback stopped (%s); waiting for cancelled reply boundary",
             unfinished_input ? "unfinished input" : (manual ? "manual" : "voice"));
}

/* speech_stopped 只是平台断句候选；先验证本地输入，再允许 worker 播放。
 * 本函数只在主任务运行，不在 WS 回调或 AFE 任务里暂停上传/发送取消。 */
static void process_reply_offer(void)
{
    ws_reply_offer_t offer;
    if (!conversation_active) return;
    if (!ws_service_get_reply_offer(&offer)) return;
    app_state_t current = app_state_get();
    if (current != APP_STATE_LISTENING && current != APP_STATE_PLAYING) return;
    if (!input_handoff.observer || input_handoff.offer.generation != offer.generation ||
        input_handoff.offer.turn != offer.turn) {
        reset_input_handoff();
        input_handoff.offer = offer;
        input_handoff.started_ms = now_ms();
        input_handoff.observer = wake_word_watch_input_handoff();
        if (!input_handoff.observer) { recover_session("could not observe input handoff"); return; }
        app_state_pause_silence_timer();
    }
    audio_in_handle_t input = audio_in_get_handle();
    audio_in_upload_status_t upload;
    audio_in_get_upload_status(input, &upload);
    if (current == APP_STATE_LISTENING && !upload.running) {
        recover_session("input ended before reply handoff"); return;
    }
    bool speech_pending, speech_confirmed;
    if (!wake_word_get_input_handoff(input_handoff.observer, &speech_pending, &speech_confirmed)) {
        reset_input_handoff(); return;
    }
    input_handoff_action_t action = input_handoff_decide(upload.running, upload.replaying,
        upload.speech_end, upload.sent_end, speech_pending, speech_confirmed);
    if (action != INPUT_HANDOFF_DRAIN && upload.running) {
        /* 接受回答前也暂停并重新检查，处理“刚检查完，AFE 又来了一帧”的窗口。
         * 暂停保留半包及 FIFO；需要继续补传时恢复原任务，不调用 stop 清数据。 */
        if (audio_in_pause(input) != ESP_OK) { recover_session("input pause timed out"); return; }
        audio_in_get_upload_status(input, &upload);
        if (!upload.running) { recover_session("input ended while pausing"); return; }
        ws_reply_offer_t latest;
        if (!ws_service_get_reply_offer(&latest) || latest.generation != offer.generation ||
            latest.turn != offer.turn || !wake_word_get_input_handoff(input_handoff.observer,
                                                                     &speech_pending, &speech_confirmed)) {
            audio_in_start_from_ringbuf(input, wake_word_get_ringbuf());
            reset_input_handoff(); return;
        }
        action = input_handoff_decide(upload.running, upload.replaying,
            upload.speech_end, upload.sent_end, speech_pending, speech_confirmed);
        if (action == INPUT_HANDOFF_DRAIN)
            audio_in_start_from_ringbuf(input, wake_word_get_ringbuf());
    }
    if (action == INPUT_HANDOFF_CANCEL) {
        ESP_LOGI(TAG, "Fresh speech confirmed after reply offer: turn=%lu; cancel response, retain input",
                 (unsigned long)offer.turn);
        reset_input_handoff();
        interrupt_response(false);
        return;
    }
    if (action == INPUT_HANDOFF_DRAIN) {
        if (!input_handoff.logged_wait) {
            input_handoff.logged_wait = true;
            uint32_t lag = input_samples_pending(upload.speech_end, upload.sent_end)
                           ? (upload.speech_end - upload.sent_end) / 16u : 0;
            ESP_LOGI(TAG, "Reply held: turn=%lu unsent_speech_ms=%lu replaying=%d; draining input, no cancel",
                     (unsigned long)offer.turn, (unsigned long)lag, upload.replaying);
        }
        if (now_ms() - input_handoff.started_ms >= 10000) {
            recover_session("microphone input handoff did not drain");
        }
        return;
    }
    reset_input_handoff();
    if (audio_in_stop(input) != ESP_OK) { recover_session("audio stop timeout"); return; }
    app_state_pause_silence_timer();
    app_state_set(APP_STATE_PLAYING);
    response_deadline_ms = now_ms() + 45000;
    if (!ws_service_accept_reply(&offer)) {
        /* 停任务期间又收到新回合/断线，旧批准不能放行新音频；继续交接新输入。 */
        resume_listening(true, "reply offer superseded");
        return;
    }
    ESP_LOGI(TAG, "Input handoff complete: turn=%lu; waiting for reply audio",
             (unsigned long)offer.turn);
}

static esp_err_t start_saved_wifi(void)
{
    char ssid[33], password[65];
    cfg_get_wifi_credential(ssid, sizeof(ssid), password, sizeof(password));
    if (!ssid[0]) return ESP_ERR_NOT_FOUND;
    esp_err_t err = wifi_service_start(ssid, password);
    if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    return err;
}

static void enter_provisioning(const char *reason)
{
    if (provisioning_service_is_active()) return;
    end_conversation(reason);
    esp_err_t err = provisioning_service_start();
    if (err == ESP_OK) {
        app_state_set(APP_STATE_PROVISIONING);
        led_set_connecting();
        ESP_LOGI(TAG, "Wi-Fi setup active; submit credentials on the local web page");
    } else {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(err));
        (void)start_saved_wifi();
    }
}

static void leave_provisioning(const char *reason)
{
    if (!provisioning_service_is_active()) return;
    esp_err_t err = provisioning_service_stop();
    app_state_back_to_wait_wake(false);
    led_set_wifi_err();
    ESP_LOGI(TAG, "Wi-Fi setup closed: %s; reconnecting saved network", reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi setup stop failed: %s", esp_err_to_name(err));
        return;
    }
    if (start_saved_wifi() == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved network; hold BOOT for 3s to configure Wi-Fi");
    }
}

static void change_volume(bool increase)
{
    int current = audio_out_get_volume();
    int next = button_volume_next(current, increase);
    if (next == current) return;
    esp_err_t err = audio_out_set_volume(next);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Volume change failed: %s", esp_err_to_name(err));
        return;
    }
    /* 连按只修改 RAM/Codec，停止调节 1.5 秒后合并写一次 NVS。 */
    s_volume_save_at_ms = now_ms() + 1500;
    ESP_LOGI(TAG, "Volume: %d -> %d", current, next);
}

static void save_volume_if_due(int64_t now)
{
    if (!s_volume_save_at_ms || now < s_volume_save_at_ms) return;
    int volume = audio_out_get_volume();
    if (volume == s_saved_volume) { s_volume_save_at_ms = 0; return; }
    esp_err_t err = cfg_service_set_int(CFG_KEY_VOLUME, volume);
    if (err == ESP_OK) {
        s_saved_volume = volume;
        s_volume_save_at_ms = 0;
        ESP_LOGI(TAG, "Volume saved: %d", volume);
    } else {
        s_volume_save_at_ms = now + 5000;
        ESP_LOGW(TAG, "Volume save failed; retry in 5s: %s", esp_err_to_name(err));
    }
}

static void change_brightness(bool increase)
{
    if (!s_ui_ready) return;
    int next = ui_brightness_next(s_brightness, increase);
    if (next == s_brightness) return;
    esp_err_t err = display_service_set_brightness(next);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brightness change failed: %s", esp_err_to_name(err));
        return;
    }
    s_brightness = next;
    s_brightness_save_at_ms = now_ms() + 1500;
    ESP_LOGI(TAG, "Brightness: %d%%", next);
}

static void save_brightness_if_due(int64_t now)
{
    if (!s_brightness_save_at_ms || now < s_brightness_save_at_ms) return;
    if (s_brightness == s_saved_brightness) { s_brightness_save_at_ms = 0; return; }
    esp_err_t err = cfg_service_set_int(CFG_KEY_BRIGHTNESS, s_brightness);
    if (err == ESP_OK) {
        s_saved_brightness = s_brightness;
        s_brightness_save_at_ms = 0;
        ESP_LOGI(TAG, "Brightness saved: %d%%", s_brightness);
    } else {
        s_brightness_save_at_ms = now + 5000;
        ESP_LOGW(TAG, "Brightness save failed; retry in 5s: %s", esp_err_to_name(err));
    }
}

/* 业务状态由主任务生成快照；UI 不读取会话变量，也不持有网络/NVS 锁。
 * 平台 HTTP 握手接入时，在这里增加状态映射即可，绘图模块不依赖握手实现。 */
static void update_ui_snapshot(int64_t now)
{
    static int64_t next_update;
    if (!s_ui_ready || now < next_update) return;
    next_update = now + 150;
    ui_snapshot_t snapshot = {0};
    snapshot.volume = audio_out_get_volume();
    snapshot.brightness = s_brightness;
    snapshot.provisioning = provisioning_service_is_active();
    snapshot.wifi_connected = wifi_service_get_ssid(snapshot.ssid, sizeof(snapshot.ssid));
    wifi_state_t wifi = wifi_service_get_state();
    snapshot.wifi_connecting = wifi == WIFI_STATE_CONNECTING || wifi == WIFI_STATE_CONNECTED;
    if (snapshot.provisioning) {
        snapshot.status = UI_STATUS_PROVISIONING;
        provisioning_service_get_info(snapshot.ap_ssid, sizeof(snapshot.ap_ssid),
                                      snapshot.provisioning_url, sizeof(snapshot.provisioning_url));
    } else if (!snapshot.wifi_connected) {
        snapshot.status = snapshot.wifi_connecting ? UI_STATUS_WIFI_CONNECTING : UI_STATUS_OFFLINE;
    } else {
        switch (app_state_get()) {
        case APP_STATE_LISTENING: snapshot.status = UI_STATUS_LISTENING; break;
        case APP_STATE_PLAYING:
            snapshot.status = audio_out_is_streaming() ? UI_STATUS_SPEAKING : UI_STATUS_WAIT_REPLY;
            break;
        case APP_STATE_CANCELLING: snapshot.status = UI_STATUS_CANCELLING; break;
        case APP_STATE_WS_CONNECTING:
        case APP_STATE_WS_CONNECTING_WAKE: snapshot.status = UI_STATUS_CONNECTING; break;
        case APP_STATE_ERROR: snapshot.status = UI_STATUS_ERROR; break;
        default: snapshot.status = session_requested ? UI_STATUS_CONNECTING : UI_STATUS_WAIT_WAKE; break;
        }
    }
    ui_service_update(&snapshot);
}

static bool tagged_ws_event(event_type_t id)
{
    return id == EV_WS_CONNECTED || id == EV_WS_AUTH_OK || id == EV_WS_ERROR ||
           id == EV_WS_DISCONNECTED || id == EV_WS_SERVER_ERROR || id == EV_WS_CANCEL_DONE ||
           id == EV_WS_SPEECH_STARTED || id == EV_WS_SPEECH_STOPPED ||
           id == EV_WS_RESPONSE_DONE || id == EV_WS_RESPONSE_CONTINUED ||
           id == EV_AUDIO_UPLOAD_FAILED;
}

static void main_task(void *arg)
{
    (void)arg;
    conversation_active = false;
    resume_deadline_ms = 0;
    ESP_LOGI(TAG, "Boot session=%08lx reset_reason=%d build=pcm-playback-v1; fresh wake required",
             (unsigned long)esp_random(), esp_reset_reason());
    app_state_set(APP_STATE_WAIT_WAKE);
    led_set_wifi_err();
    if (start_saved_wifi() != ESP_OK) enter_provisioning("no usable Wi-Fi configuration");

    while (1) {
        event_t event;
        if (event_bus_receive(s_bus, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (tagged_ws_event(event.type) &&
                ((uint32_t)(uintptr_t)event.payload != ws_service_generation() ||
                 !session_requested)) continue;
            app_state_t current_state = app_state_get();
            switch (event.type) {
            case EV_WIFI_GOT_IP:
                if (provisioning_service_is_active() || wifi_service_get_state() != WIFI_STATE_GOT_IP) break;
                provisioning_service_confirm();
                /* 普通联网不打开 WS；重复 IP 事件不能打断正在进行的对话。 */
                if (!session_requested) {
                    ESP_LOGI(TAG, "Wi-Fi ready; waiting for wake word before connecting");
                    app_state_back_to_wait_wake(false);
                    led_set_waiting_wake();
                }
                break;
            case EV_PROVISIONING_DONE:
                if ((uint32_t)(uintptr_t)event.payload == provisioning_service_generation())
                    leave_provisioning("credentials submitted");
                break;
            case EV_PROVISIONING_TIMEOUT:
                if (provisioning_service_is_active() && provisioning_service_timed_out())
                    leave_provisioning("10-minute timeout");
                break;
            case EV_WIFI_DISCONNECTED:
            case EV_WIFI_CONNECT_FAILED:
                /* 配网时以及已经恢复 IP 时，忽略队列中迟到的断线通知。 */
                if (provisioning_service_is_active() || wifi_service_get_state() == WIFI_STATE_GOT_IP ||
                    wifi_service_get_state() == WIFI_STATE_IDLE) break;
                if (event.type == EV_WIFI_CONNECT_FAILED && wifi_service_get_state() != WIFI_STATE_FAIL) break;
                if (event.type == EV_WIFI_CONNECT_FAILED) {
                    provisioning_service_rollback();
                    enter_provisioning("Wi-Fi connection failed");
                } else {
                    end_conversation("Wi-Fi disconnected");
                }
                break;
            /* 按键和 UI 共用业务动作；所有网络/Codec/NVS 操作留在主任务。 */
            case EV_BUTTON_VOLUME_DOWN:
            case EV_BUTTON_VOLUME_UP:
                change_volume(event.type == EV_BUTTON_VOLUME_UP);
                break;
            case EV_UI_TALK:
            case EV_BUTTON_TALK:
                /* 首页监听画面轻触结束对话；取消阶段也显示监听，采用同一操作。
                 * 复用完整收尾入口，清除会话意图/重试期限，迟到的取消确认不能重新拉起监听。
                 * 只改变触屏行为，保留实体 REC 键的原有规则。 */
                if (event.type == EV_UI_TALK &&
                    (current_state == APP_STATE_LISTENING || current_state == APP_STATE_CANCELLING)) {
                    end_conversation("touch while listening");
                    break;
                }
                if (current_state == APP_STATE_PLAYING) interrupt_response(true);
                else {
                    start_conversation(event.type == EV_UI_TALK ? "touch" : "REC button");
                }
                break;
            case EV_UI_BRIGHTNESS_DOWN:
            case EV_UI_BRIGHTNESS_UP:
                change_brightness(event.type == EV_UI_BRIGHTNESS_UP);
                break;
            case EV_BUTTON_END_TALK:
                if (!provisioning_service_is_active()) end_conversation("REC held for 2s");
                break;
            case EV_BUTTON_PROVISION:
                enter_provisioning("BOOT held for 3s");
                break;
            case EV_UI_PROVISION:
                enter_provisioning("touch menu");
                break;
            case EV_UI_PROVISION_EXIT:
                leave_provisioning("touch menu cancelled");
                break;
            case EV_WS_CONNECTED:
                ESP_LOGI(TAG, "WebSocket transport connected; authenticating");
                auth_deadline_ms = now_ms() + 10000;
                if (ws_service_authenticate() != ESP_OK) recover_session("auth send failed");
                break;
            case EV_WS_AUTH_OK:
                auth_deadline_ms = retry_at_ms = 0;
                ESP_LOGI(TAG, "WebSocket authenticated; audio upload ready");
                conversation_active = true;
                resume_listening(false, resume_deadline_ms ? "reconnect" : "wake connect complete");
                break;
            case EV_WS_DISCONNECTED:
                recover_session("WebSocket transport disconnected");
                break;
            case EV_WS_ERROR:
                recover_session("WebSocket transport/receive failure");
                break;
            case EV_WS_SERVER_ERROR:
                recover_session("server protocol error (see ws_service error code)");
                break;
            case EV_AUDIO_UPLOAD_FAILED:
                recover_session("audio upload failed");
                break;
            case EV_AUDIO_WAKE_DETECTED:
                start_conversation("wake word");
                break;
            case EV_AUDIO_SPEECH_START:
            case EV_WS_SPEECH_STARTED:
                if (conversation_active && current_state == APP_STATE_LISTENING)
                    app_state_reset_silence_timer();
                break;
            case EV_WS_SPEECH_STOPPED:
            case EV_WS_RESPONSE_CONTINUED:
                /* worker 已设交接门；下方轮询当前 offer，避免迟到事件关掉新输入。 */
                break;
            case EV_AUDIO_OUT_START:
                if (conversation_active && current_state == APP_STATE_PLAYING &&
                    ws_service_get_state() == WS_STATE_CONNECTED) {
                    response_deadline_ms = now_ms() + 180000;
                    led_set_speaking();
                }
                break;
            case EV_AUDIO_BARGE_IN:
                interrupt_response(false);
                break;
            case EV_WS_CANCEL_DONE:
                if (conversation_active && current_state == APP_STATE_CANCELLING) {
                    ESP_LOGI(TAG, "Cancel acknowledged; resume buffered microphone audio");
                    resume_listening(true, "barge-in completed");
                }
                break;
            case EV_WS_RESPONSE_DONE:
                if (conversation_active && current_state == APP_STATE_PLAYING) {
                    ESP_LOGI(TAG, "Reply complete; continue conversation");
                    resume_listening(false, "reply completed");
                }
                break;
            case EV_AUDIO_SILENCE_TIMEOUT:
                /* Ignore an already queued timeout from before playback. */
                if (current_state != APP_STATE_LISTENING) break;
                if (esp_timer_is_active(app_state_get_global()->silence_timer)) break;
                end_conversation("60s silence timeout");
                break;
            case EV_AUDIO_INPUT_ERROR:
                recover_session("audio input error");
                break;
            default: break;
            }
        }
        int64_t now = now_ms();
        process_reply_offer();
        save_volume_if_due(now);
        save_brightness_if_due(now);
        update_ui_snapshot(now);
        if (cancel_probe_ms && now >= cancel_probe_ms) {
            cancel_probe_ms = 0;
            ESP_LOGW(TAG, "Cancel confirmation delayed; keeping connection while waiting (limit 8 s)");
            ws_service_log_cancel_wait();
        }
        if (resume_deadline_ms && now >= resume_deadline_ms) {
            end_conversation("conversation recovery expired");
        }
        if (cancel_deadline_ms && now >= cancel_deadline_ms) {
            ws_service_log_cancel_wait();
            recover_session("cancel acknowledgement timed out");
        }
        else if (response_deadline_ms && now >= response_deadline_ms) {
            ws_service_log_response_wait();
            recover_session("server reply timed out");
        }
        else if (auth_deadline_ms && now >= auth_deadline_ms)
            recover_session("connection/authentication timed out");
        if (session_requested && retry_at_ms && now >= retry_at_ms &&
            wifi_service_get_state() == WIFI_STATE_GOT_IP && !provisioning_service_is_active()) {
            ESP_LOGI(TAG, "Connecting WebSocket (wake-triggered or retry) ...");
            /* Stop is idempotent, and retries a prior failed stop before start. */
            esp_err_t err = ws_service_disconnect();
            if (err == ESP_OK) err = ws_service_connect();
            if (err == ESP_OK) {
                retry_at_ms = 0;
                auth_deadline_ms = now_ms() + 20000;
            } else retry_at_ms = now_ms() + 5000;
        }
        led_service_poll(s_led, esp_log_timestamp());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* ── 应用入口 ───────────────────────────────────────────────── */

void app_main(void)
{
    s_bus = event_bus_create(32);
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        abort();
    }

    if (system_init() != ESP_OK) {
        ESP_LOGE(TAG, "system_init failed, rebooting in 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    xTaskCreatePinnedToCore(main_task, "main", MAIN_TASK_STACK,
                            NULL, MAIN_TASK_PRIORITY, NULL, 0);

    ESP_LOGI(TAG, "app_main done, main_task running on core 0");
}
