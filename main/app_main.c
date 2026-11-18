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
#include "device_handshake_service.h"
#include "device_mqtt_service.h"
#include "log_service.h"
#include "led_service.h"
#include "audio_out.h"
#include "ws_service.h"
#include "proto_service.h"
#include "audio_in.h"
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
    ESP_LOGI(TAG, "=== YMOS VoCat v1.2 starting (ESP-IDF v6.1, touchscreen, device MQTT) ===");

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
/* 提醒独占一次临时 WS 会话，结束回待机。主任务不改 MQTT/NVS 内部状态。 */
static device_mqtt_delivery_t *reminder_task;
static bool reminder_requested;
static int64_t reminder_deadline_ms;
/* 每次主动唤醒最多因鉴权失败重新握手一次；普通网络恢复不消耗该次数。 */
static bool voice_auth_refresh_used, awaiting_voice_auth, waiting_for_credentials;
static int64_t retry_at_ms, auth_deadline_ms, response_deadline_ms;
static int64_t resume_deadline_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void end_conversation(const char *reason)
{
    device_mqtt_service_set_dispatch_allowed(false);
    if (reminder_task) {
        device_mqtt_service_finish_tasks(reminder_task,
            wifi_service_get_state() != WIFI_STATE_GOT_IP ? DEVICE_TASK_NETWORK_WAIT : DEVICE_TASK_FAILED);
        reminder_task = NULL;
    }
    reminder_requested = false;
    reminder_deadline_ms = 0;
    session_requested = conversation_active = false;
    voice_auth_refresh_used = awaiting_voice_auth = waiting_for_credentials = false;
    retry_at_ms = auth_deadline_ms = response_deadline_ms = 0;
    resume_deadline_ms = 0;
    /* 先停上传/播放，再启用 WakeNet，避免结束时把自己的尾音当成新唤醒。 */
    app_state_pause_silence_timer();
    esp_err_t input_err = audio_in_stop(audio_in_get_handle());
    if (input_err != ESP_OK) ESP_LOGW(TAG, "Audio stop failed: %s", esp_err_to_name(input_err));
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop failed: %s", esp_err_to_name(err));
    app_state_back_to_wait_wake(false);
    wake_word_clear_audio_buffer();
    wake_word_set_audio_buffer_enabled(true);
    if (wifi_service_get_state() == WIFI_STATE_GOT_IP) led_set_waiting_wake();
    else led_set_wifi_err();
    ESP_LOGI(TAG, "Conversation ended: %s; waiting for wake word", reason);
}

static void recover_session(const char *reason)
{
    if (!session_requested || provisioning_service_is_active()) return;
    /* 提醒可能已经发出/部分播出；失败不能自行重发并声称恰好一次。 */
    if (reminder_task) { end_conversation(reason); return; }
    ESP_LOGW(TAG, "Session recovery: %s", reason);
    if (conversation_active && !resume_deadline_ms) resume_deadline_ms = now_ms() + 60000;
    app_state_back_to_wait_wake(false);
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop pending: %s", esp_err_to_name(err));
    wake_word_clear_audio_buffer();
    auth_deadline_ms = response_deadline_ms = 0;
    awaiting_voice_auth = waiting_for_credentials = false;
    retry_at_ms = now_ms() + 5000;
    ESP_LOGW(TAG, "Reconnect scheduled in 5000 ms; resume_conversation=%d (no voice required)",
             conversation_active);
    led_set_ws_err();
}

static void handle_voice_auth_failure(const char *reason)
{
    if (!session_requested) return;
    if (reminder_task) { end_conversation(reason); return; }
    if (!device_handshake_service_is_enabled() || voice_auth_refresh_used) {
        ESP_LOGE(TAG, "Voice authentication failed; automatic retries stopped (check gateway auth protocol)");
        end_conversation(reason);
        return;
    }
    voice_auth_refresh_used = true;
    recover_session(reason);
    if (device_handshake_service_renew_credentials())
        ESP_LOGW(TAG, "Voice authentication rejected; requesting fresh device token once");
    /* 若 HTTP 已因 401/403 等停止，下一次 prepare 会返回 BLOCKED，不能强行 refresh 解锁。 */
}

static void retry_voice_connection(int64_t now)
{
    if (!session_requested || !retry_at_ms || now < retry_at_ms ||
        wifi_service_get_state() != WIFI_STATE_GOT_IP || provisioning_service_is_active()) return;
    const device_handshake_response_t *configuration = NULL;
    device_ws_readiness_t readiness = device_handshake_service_prepare_websocket(&configuration);
    if (readiness == DEVICE_WS_BLOCKED) {
        end_conversation("device handshake blocked; check configuration/platform");
        return;
    }
    if (readiness == DEVICE_WS_WAITING) {
        if (!waiting_for_credentials) {
            waiting_for_credentials = true;
            ESP_LOGI(TAG, "Waiting for valid HTTP handshake credentials before WebSocket");
        }
        return; /* 只等待后台结果，不反复 stop，不占用音频/HTTP 工作任务。 */
    }
    waiting_for_credentials = false;
    ESP_LOGI(TAG, "Connecting WebSocket; credentials=%s",
             readiness == DEVICE_WS_READY ? "device handshake" : "legacy NVS (handshake disabled)");
    /* 先停旧连接，再整组复制配置；下一次 poll 清除握手快照不会影响本连接。 */
    esp_err_t err = ws_service_disconnect();
    if (err == ESP_OK) err = ws_service_set_credentials(
        configuration ? configuration->websocket.url : NULL,
        configuration ? configuration->websocket.token : NULL);
    if (err == ESP_OK && reminder_task) {
        const char *task_ids[DEVICE_TASK_QUEUE_SIZE];
        for (unsigned i = 0; i < reminder_task->count; ++i)
            task_ids[i] = reminder_task->task_ids[i];
        err = ws_service_prepare_reminder(reminder_task->device_id, task_ids, reminder_task->count);
    }
    if (err == ESP_OK) err = ws_service_connect();
    if (err == ESP_OK) {
        retry_at_ms = 0;
        awaiting_voice_auth = false;
        auth_deadline_ms = now_ms() + 20000;
    } else {
        ESP_LOGW(TAG, "WebSocket prepare/start failed: %s", esp_err_to_name(err));
        if (reminder_task) { end_conversation("reminder connection failed"); return; }
        retry_at_ms = now_ms() + 5000;
    }
}

static void resume_listening(bool keep_buffered_speech, const char *reason)
{
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
    response_deadline_ms = 0;
    if (app_state_start_listening() != ESP_OK) {
        recover_session("could not start audio upload");
        return;
    }

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
    device_mqtt_service_on_wake();
    device_mqtt_service_set_dispatch_allowed(false);
    if (reminder_task) end_conversation("user wake during reminder");
    if (session_requested) {
        if (app_state_get() == APP_STATE_LISTENING) app_state_reset_silence_timer();
        return; /* 连接中或会话中重复按键不重复发送请求。 */
    }
    session_requested = true;
    voice_auth_refresh_used = awaiting_voice_auth = waiting_for_credentials = false;
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

/* 平台断句后只放行下行，不暂停/停止麦克风。新的发言由平台判定，
 * 设备不再根据本地 VAD 或尚未上传的缓存取消回答。 */
static void process_reply_offer(void)
{
    ws_reply_offer_t offer;
    if (!conversation_active || !ws_service_get_reply_offer(&offer)) return;
    app_state_t current = app_state_get();
    if (current != APP_STATE_LISTENING && current != APP_STATE_PLAYING) return;
    audio_in_upload_status_t upload;
    audio_in_get_upload_status(audio_in_get_handle(), &upload);
    if (!upload.running) { recover_session("continuous audio upload stopped"); return; }
    if (!ws_service_accept_reply(&offer)) return; /* 旧批准不能放行新回合。 */
    app_state_pause_silence_timer();
    app_state_set(APP_STATE_PLAYING);
    response_deadline_ms = now_ms() + 45000;
    ESP_LOGI(TAG, "Platform reply accepted: turn=%lu; microphone upload continues",
             (unsigned long)offer.turn);
}

/* worker 已停止并隔离旧播放；这里只更新 UI/静默计时，保持同一上传任务。
 * 查询当前阶段，防止迟到事件把已开始的新回答切回监听。 */
static void process_platform_interruption(void)
{
    if (!conversation_active || !ws_service_reply_interrupted()) return;
    resume_listening(true, "platform interruption; microphone retained");
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
           id == EV_WS_DISCONNECTED || id == EV_WS_SERVER_ERROR || id == EV_WS_AUTH_FAILED || id == EV_WS_RESPONSE_INTERRUPTED ||
           id == EV_WS_SPEECH_STARTED || id == EV_WS_SPEECH_STOPPED ||
           id == EV_WS_RESPONSE_DONE || id == EV_WS_RESPONSE_CONTINUED ||
           id == EV_AUDIO_UPLOAD_FAILED;
}

/* 提醒不打断活跃对话；空闲时批量起独立 WS 会话，由智能体按 task_ids 取内容。 */
static void poll_reminder(void)
{
    if (reminder_task) {
        if (now_ms() >= reminder_deadline_ms ||
            (reminder_requested && ws_service_reminder_timed_out()))
            end_conversation("reminder request/playback timed out");
        return;
    }
    if (session_requested || provisioning_service_is_active() ||
        wifi_service_get_state() != WIFI_STATE_GOT_IP ||
        app_state_get() != APP_STATE_WAIT_WAKE || !device_mqtt_service_voice_enabled()) return;
    reminder_task = device_mqtt_service_take_tasks();
    if (!reminder_task) return;
    device_mqtt_service_set_dispatch_allowed(false);
    session_requested = true;
    conversation_active = false;
    reminder_requested = false;
    voice_auth_refresh_used = awaiting_voice_auth = waiting_for_credentials = false;
    app_state_pause_silence_timer();
    reminder_deadline_ms = now_ms() + 360000;
    app_state_set(APP_STATE_REMINDER_WAITING);
    wake_word_set_audio_buffer_enabled(false);
    /* 提醒只下行播放；关闭 WakeNet 避免播报内容唤醒自己，不启动麦克风上传。
     * 复用现有 Wi-Fi 音频模式，结束路径的 audio_in_stop 会恢复省电。 */
    if (wake_word_set_mode(WAKE_WORD_MODE_LITE) != ESP_OK) {
        end_conversation("reminder AFE setup failed");
        return;
    }
    (void)wifi_service_set_audio_active(true);
    retry_at_ms = now_ms();
    ESP_LOGI(TAG, "Reminder selected: task_count=%u; connecting without microphone upload",
             reminder_task->count);
}

static void main_task(void *arg)
{
    (void)arg;
    /* 后台 HTTP 负责获取凭据；开启后语音连接必须通过有效快照，不回退到旧 key。 */
    esp_err_t handshake_err = device_handshake_service_init();
    if (handshake_err != ESP_OK)
        ESP_LOGW(TAG, "Device HTTP handshake unavailable: %s", esp_err_to_name(handshake_err));
    esp_err_t mqtt_err = device_mqtt_service_init();
    if (mqtt_err != ESP_OK)
        ESP_LOGW(TAG, "Device MQTT unavailable: %s", esp_err_to_name(mqtt_err));
    conversation_active = false;
    resume_deadline_ms = 0;
    ESP_LOGI(TAG, "Boot session=%08lx reset_reason=%d build=platform-barge-in-v1; fresh wake required",
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
                if (reminder_task) {
                    end_conversation("reminder cancelled by touch/button");
                    break;
                }
                /* 首页监听画面轻触结束对话；保留实体 REC 键的原有唤醒规则。 */
                if (event.type == EV_UI_TALK && current_state == APP_STATE_LISTENING) {
                    end_conversation("touch while listening");
                    break;
                }
                /* 播放中轻触不再触发本地打断；监听时轻触结束对话的规则保留。 */
                if (current_state == APP_STATE_PLAYING)
                    ESP_LOGI(TAG, "Talk ignored during playback; interruption is platform controlled");
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
                awaiting_voice_auth = true;
                auth_deadline_ms = now_ms() + 10000;
                if (ws_service_authenticate() != ESP_OK) recover_session("auth send failed");
                break;
            case EV_WS_AUTH_OK:
                auth_deadline_ms = retry_at_ms = 0;
                awaiting_voice_auth = false;
                if (reminder_task) {
                    /* auth 首帧已携带 task_ids；无 listening/麦克风上传，等待智能体下行音频。 */
                    app_state_set(APP_STATE_PLAYING);
                    reminder_requested = true;
                    ESP_LOGI(TAG, "Reminder session authenticated; waiting for agent audio: task_count=%u",
                             reminder_task->count);
                    break;
                }
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
            case EV_WS_AUTH_FAILED:
                handle_voice_auth_failure("gateway authentication rejected");
                break;
            case EV_AUDIO_UPLOAD_FAILED:
                recover_session("audio upload failed");
                break;
            case EV_AUDIO_WAKE_DETECTED:
                start_conversation("wake word");
                break;
            case EV_AUDIO_SPEECH_START:
                if (conversation_active && current_state == APP_STATE_LISTENING)
                    app_state_reset_silence_timer();
                break;
            case EV_WS_SPEECH_STARTED:
            case EV_WS_RESPONSE_INTERRUPTED:
                process_platform_interruption();
                if (conversation_active && app_state_get() == APP_STATE_LISTENING)
                    app_state_reset_silence_timer();
                break;
            case EV_WS_SPEECH_STOPPED:
            case EV_WS_RESPONSE_CONTINUED:
                /* worker 已设播放门；下方轮询当前 offer，麦克风始终上传。 */
                break;
            case EV_AUDIO_OUT_START:
                if ((conversation_active || (reminder_task && reminder_requested)) &&
                    current_state == APP_STATE_PLAYING &&
                    ws_service_get_state() == WS_STATE_CONNECTED) {
                    if (conversation_active) response_deadline_ms = now_ms() + 180000;
                    led_set_speaking();
                }
                break;
            case EV_WS_RESPONSE_DONE:
                if (reminder_task && reminder_requested) {
                    /* 事件排队期间可能已有下一段 created；不能提前确认整批任务。 */
                    if (!ws_service_reply_finished()) break;
                    /* 只有音频边界完成且 PCM 排空后，服务才允许持久化并回执。 */
                    device_mqtt_service_finish_tasks(reminder_task, DEVICE_TASK_AUDIO_RECEIVED);
                    reminder_task = NULL;
                    end_conversation("reminder playback completed");
                    break;
                }
                if (conversation_active && current_state == APP_STATE_PLAYING &&
                    ws_service_reply_finished()) {
                    ESP_LOGI(TAG, "Reply complete; continue conversation");
                    resume_listening(true, "reply completed");
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
        device_handshake_service_poll(wifi_service_get_state() == WIFI_STATE_GOT_IP &&
                                      !provisioning_service_is_active());
        device_mqtt_service_set_dispatch_allowed(
            wifi_service_get_state() == WIFI_STATE_GOT_IP &&
            !provisioning_service_is_active() && !session_requested &&
            app_state_get() == APP_STATE_WAIT_WAKE);
        device_mqtt_service_poll(device_handshake_service_is_enabled() && !device_handshake_service_is_blocked() &&
            wifi_service_get_state() == WIFI_STATE_GOT_IP && !provisioning_service_is_active(),
            device_handshake_service_mqtt_epoch(), device_handshake_service_get_configuration());
        poll_reminder();
        process_reply_offer();
        save_volume_if_due(now);
        save_brightness_if_due(now);
        update_ui_snapshot(now);
        if (resume_deadline_ms && now >= resume_deadline_ms) {
            end_conversation("conversation recovery expired");
        }
        if (response_deadline_ms && now >= response_deadline_ms) {
            ws_service_log_response_wait();
            recover_session("server reply timed out");
        }
        else if (auth_deadline_ms && now >= auth_deadline_ms) {
            if (awaiting_voice_auth) handle_voice_auth_failure("gateway authentication timed out");
            else recover_session("WebSocket connection timed out");
        }
        retry_voice_connection(now);
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
