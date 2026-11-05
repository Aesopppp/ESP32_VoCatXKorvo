/**
 * @file led_service.c
 * @brief 保留 YMOS 的逻辑状态模式；VoCat 无屏版没有实体 LED 输出。
 */

#include "led_service.h"
#include <stdlib.h>
#include "esp_log.h"
#include "event_bus.h"

static const char *TAG = "led_service";

/* ── 全局单例 ──────────────────────────────────────────────── */
static led_service_t s_led_service = NULL;


/* ── 应用层模式宏（简化 app_main 调用） ─────────────────────── */

/**
 * Wi-Fi 连接成功: 绿常亮
 * @return ESP_OK on success
 */
esp_err_t led_set_wifi_ok(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_ON);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * Wi-Fi 连接失败: 红快闪
 */
esp_err_t led_set_wifi_err(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_BLINK_FAST);
    return ESP_OK;
}

/**
 * WebSocket 已连接: 绿慢闪
 */
esp_err_t led_set_ws_ok(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * WebSocket 断开/错误: 红慢闪
 */
esp_err_t led_set_ws_err(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_BLINK_SLOW);
    return ESP_OK;
}

/**
 * 等待唤醒词: 绿呼吸
 */
esp_err_t led_set_waiting_wake(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BREATHE);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}


/**
 * 唤醒后建连中 (APP_STATE_WS_CONNECTING_WAKE): 红绿交替快闪
 * 表明:已检测到唤醒词,正在连接服务器,请稍候再说。
 */
esp_err_t led_set_connecting(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_FAST);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_BLINK_FAST);
    return ESP_OK;
}
/**
 * 正在录音/说话: 绿快闪
 */
esp_err_t led_set_listening(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_FAST);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * 放音中: 绿慢闪
 */
esp_err_t led_set_speaking(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/* 本版没有 Korvo 的红/绿灯。保留状态服务 API，方便未来触屏接入；
 * 不驱动 VoCat GPIO43（原板 LED/UART 复用），也不创建额外任务。
 */
struct led_service_impl {
    led_state_t leds[LED_COUNT];
    event_bus_t bus;
};
#define BLINK_FAST_INTERVAL_MS 100
#define BLINK_SLOW_INTERVAL_MS 500

led_service_t led_service_init(void)
{
    if (s_led_service) return s_led_service;
    s_led_service = calloc(1, sizeof(struct led_service_impl));
    if (!s_led_service) return NULL;
    for (int i = 0; i < LED_COUNT; ++i) s_led_service->leds[i].enabled = true;
    ESP_LOGI(TAG, "Logical status service ready; no physical status LED in this port");
    return s_led_service;
}

void led_service_destroy(led_service_t svc)
{
    if (svc == s_led_service) s_led_service = NULL;
    free(svc);
}

static void led_apply_state(led_service_t svc)
{
    (void)svc; /* 后续显示适配只需替换此输出端，不在此控制板级电源。 */
}

void led_service_poll(led_service_t svc, uint32_t now_ms)
{
    if (svc == NULL) return;

    bool changed = false;

    for (int i = 0; i < LED_COUNT; i++) {
        if (!svc->leds[i].enabled) continue;

        switch (svc->leds[i].pattern) {
            case LED_PATTERN_OFF:
                if (svc->leds[i].current_state != false) {
                    svc->leds[i].current_state = false;
                    changed = true;
                }
                break;

            case LED_PATTERN_ON:
                if (svc->leds[i].current_state != true) {
                    svc->leds[i].current_state = true;
                    changed = true;
                }
                break;

            case LED_PATTERN_BLINK_FAST:
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= BLINK_FAST_INTERVAL_MS) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;

            case LED_PATTERN_BLINK_SLOW:
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= BLINK_SLOW_INTERVAL_MS) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;

            case LED_PATTERN_BREATHE:
                /* 简化: 慢闪代替呼吸 */
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= 800) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;
        }
    }

    if (changed) {
        led_apply_state(svc);
    }
}

esp_err_t led_service_set_pattern(led_service_t svc, led_id_t id, led_pattern_t pattern)
{
    if (svc == NULL || id >= LED_COUNT) return ESP_ERR_INVALID_ARG;

    svc->leds[id].pattern = pattern;
    svc->leds[id].last_toggle_ms = 0;
    svc->leds[id].current_state = false;

    ESP_LOGD(TAG, "led[%d] set pattern=%d", id, pattern);

    /* 立即应用 */
    led_apply_state(svc);
    return ESP_OK;
}

esp_err_t led_service_idle(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_ON);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_recording(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_BLINK_FAST);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_playing(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_error(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_BLINK_FAST);
    return ESP_OK;
}

esp_err_t led_service_off(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/* 事件总线处理函数 */
static void on_event_bus(event_t *event, void *user_data)
{
    led_service_t svc = (led_service_t)user_data;
    if (svc == NULL) return;

    switch (event->type) {
        case EV_WS_AUTH_OK:
            led_service_idle(svc);
            break;
        case EV_AUDIO_SPEECH_START:
            led_service_recording(svc);
            break;
        case EV_AUDIO_SPEECH_END:
            led_service_idle(svc);
            break;
        case EV_WS_DISCONNECTED:
        case EV_SYSTEM_WIFI_DISCONNECTED:
            led_service_off(svc);
            break;
        case EV_AUDIO_OUT_START:
            led_service_playing(svc);
            break;
        case EV_AUDIO_BARGE_IN:
            led_service_recording(svc);
            break;
        case EV_SYSTEM_ERROR:
        case EV_WS_ERROR:
            led_service_error(svc);
            break;
        default:
            break;
    }
}

void led_service_register_events(led_service_t svc)
{
    if (svc == NULL || svc->bus == NULL) return;

    event_type_t events[] = {
        EV_WS_AUTH_OK, EV_WS_DISCONNECTED, EV_WS_ERROR,
        EV_SYSTEM_WIFI_DISCONNECTED,
        EV_AUDIO_SPEECH_START, EV_AUDIO_SPEECH_END,
        EV_AUDIO_OUT_START, EV_AUDIO_BARGE_IN,
        EV_SYSTEM_ERROR,
    };

    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        event_bus_subscribe(svc->bus, events[i], on_event_bus, svc);
    }
}
