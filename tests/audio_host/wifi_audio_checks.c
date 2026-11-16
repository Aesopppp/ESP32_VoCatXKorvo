/* 执行生产 Wi-Fi 模式 API 和事件处理器；仅驱动/RTOS 依赖使用桩函数。 */
#include <stdio.h>
#include <stdlib.h>
#include "stdatomic.h"
#include "esp_log.h"
#include "../../main/wifi_service.h"

static unsigned checks, lock_depth, mode_calls, connect_calls;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM } wifi_ps_type_t;
typedef int esp_event_base_t;
enum { WIFI_EVENT = 1, IP_EVENT, WIFI_EVENT_STA_START, WIFI_EVENT_STA_CONNECTED,
       WIFI_EVENT_STA_DISCONNECTED, IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP };
typedef struct { int reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { int ip; } ip_info; } ip_event_got_ip_t;
#define ESP_ERR_WIFI_CONN 20
#define WIFI_GOT_IP_BIT 2
#define WIFI_CONNECT_TIMEOUT_MS 60000
static int wifi_mutex;
static void *s_mutex;
static void *s_wifi_event_group, *s_connect_timer;
static wifi_state_t s_wifi_state;
static atomic_bool s_connect_enabled;
static bool s_audio_active, timer_active;
static int64_t s_connect_deadline_us;
static wifi_ps_type_t applied_mode = WIFI_PS_MIN_MODEM;
static esp_err_t mode_result;
static void lock(void) { CHECK(s_mutex != NULL); ++lock_depth; }
static void unlock(void) { CHECK(lock_depth > 0); --lock_depth; }
static void set_state(wifi_state_t state) { s_wifi_state = state; }
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode)
{ CHECK(lock_depth > 0); ++mode_calls; if (!mode_result) applied_mode = mode; return mode_result; }
static esp_err_t esp_wifi_connect(void) { ++connect_calls; return ESP_OK; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static bool esp_timer_is_active(void *timer) { (void)timer; return timer_active; }
static void esp_timer_start_once(void *timer, uint64_t us)
{ (void)timer; CHECK(us == 60000000); timer_active = true; }
static void esp_timer_stop(void *timer) { (void)timer; timer_active = false; }
static void xEventGroupClearBits(void *group, unsigned bits) { (void)group; (void)bits; }
static void xEventGroupSetBits(void *group, unsigned bits) { (void)group; (void)bits; }
static void led_set_wifi_err(void) {}
static void led_set_wifi_ok(void) {}
#include "wifi_audio.inc"

int main(void)
{
    CHECK(wifi_service_set_audio_active(true) == ESP_ERR_INVALID_STATE);
    CHECK(mode_calls == 0);
    s_mutex = &wifi_mutex;
    s_wifi_state = WIFI_STATE_IDLE;
    CHECK(wifi_service_set_audio_active(true) == ESP_OK && mode_calls == 0);
    CHECK(s_audio_active);
    s_connect_enabled = true;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    CHECK(applied_mode == WIFI_PS_NONE && s_wifi_state == WIFI_STATE_CONNECTED);
    s_wifi_state = WIFI_STATE_GOT_IP;
    CHECK(wifi_service_set_audio_active(false) == ESP_OK);
    CHECK(applied_mode == WIFI_PS_MIN_MODEM);
    CHECK(wifi_service_set_audio_active(true) == ESP_OK);
    CHECK(applied_mode == WIFI_PS_NONE);

    wifi_event_sta_disconnected_t disconnected = {.reason = 200};
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    CHECK(s_wifi_state == WIFI_STATE_DISCONNECTED && s_audio_active);
    CHECK(connect_calls == 1);
    applied_mode = WIFI_PS_MIN_MODEM; /* 模拟驱动重新启动后使用默认模式。 */
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    CHECK(applied_mode == WIFI_PS_NONE);

    mode_result = ESP_FAIL;
    CHECK(wifi_service_set_audio_active(false) == ESP_FAIL);
    CHECK(!s_audio_active && s_wifi_state == WIFI_STATE_CONNECTED);
    CHECK(connect_calls == 1); /* 性能模式失败不触发重连。 */
    mode_result = ESP_OK;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    CHECK(applied_mode == WIFI_PS_MIN_MODEM);

    s_connect_enabled = false; /* AP 配网/主动停止期间不调用 STA 驱动。 */
    s_wifi_state = WIFI_STATE_IDLE;
    unsigned previous = mode_calls;
    CHECK(wifi_service_set_audio_active(true) == ESP_OK);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    CHECK(mode_calls == previous && s_wifi_state == WIFI_STATE_IDLE);
    CHECK(wifi_service_set_audio_active(false) == ESP_OK);
    CHECK(!s_audio_active && mode_calls == previous && lock_depth == 0);
    printf("PASS: %u Wi-Fi audio checks (start/stop, reconnect, failed mode, provisioning guard)\n", checks);
    return 0;
}
