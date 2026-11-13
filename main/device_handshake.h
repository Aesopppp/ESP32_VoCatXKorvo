#ifndef DEVICE_HANDSHAKE_H
#define DEVICE_HANDSHAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "proto_service.h"

/* 平台 HTTP 握手协议的纯逻辑；不依赖 Wi-Fi、NVS 或应用状态机。 */
#define DEVICE_HANDSHAKE_CONFIG_MAX 4096
#define DEVICE_HANDSHAKE_RESPONSE_MAX 16384
#define DEVICE_HANDSHAKE_PATH "/api/device/v1/handshake"

typedef struct {
    bool enabled;
    bool allow_insecure_http; /* 仅为受控 HTTP 测试显式开启。 */
    char server_url[513];
    char device_id[65];       /* 平台签发，不能用原 YMOS 的 MAC 派生 ID 代替。 */
    char device_model[65];
    char psk[513];
} device_handshake_config_t;

typedef struct {
        char endpoint[513], client_id[257], username[65], password[513];
        char publish_topic[129], subscribe_topic[129];
} device_mqtt_configuration_t;

typedef struct {
    char server_time[40];
    int64_t server_time_ms;   /* Unix UTC 毫秒；校时不叠加时区偏移。 */
    device_mqtt_configuration_t mqtt;
    struct {
        char url[PROTO_WS_URL_MAX + 1], token[PROTO_AUTH_TOKEN_MAX + 1];
        uint32_t expires_in; /* 平台返回的剩余秒数；不得自行猜测默认 TTL。 */
    } websocket;
    struct { char version[65], url[1025]; } firmware; /* null 时两项为空。 */
} device_handshake_response_t;

/* 大对象在设备上使用 PSRAM；主机测试使用普通堆。wipe 不会被优化掉。 */
void *device_handshake_alloc(size_t bytes);
void device_handshake_wipe(void *buffer, size_t bytes);
bool device_handshake_id_valid(const char *value);
bool device_handshake_parse_time(const char *text, int64_t *unix_ms);
bool device_handshake_parse_config(const char *json, size_t len,
                                  device_handshake_config_t *output, const char **reason);
bool device_handshake_parse_response(const char *json, size_t len, const char *device_id,
                                    device_handshake_response_t *output, const char **reason);
/* 返回固定白名单字符串，禁止将服务端任意错误正文作为日志。 */
const char *device_handshake_error_code(const char *json, size_t len);
bool device_handshake_retryable(int http_status);
unsigned device_handshake_retry_delay_ms(unsigned failures);

/* 仅使用单调时钟；基准取 HTTP 请求开始，保守扣除传输和主任务排队时间。
 * 时钟倒退、过期或非法基准均返回 0；不会受 server_time/SNTP 校时影响。 */
int64_t device_handshake_token_remaining_ms(uint32_t expires_in, int64_t acquired_ms, int64_t now_ms);

/* 只由应用主任务访问；网络代次过滤旧请求，Wi-Fi 重连不能解除永久错误。 */
typedef struct {
    bool online, ready, blocked;
    uint32_t generation;
    unsigned failures;
    int64_t retry_at_ms;
} device_handshake_schedule_t;

void device_handshake_network_changed(device_handshake_schedule_t *state, bool online);
void device_handshake_refresh(device_handshake_schedule_t *state);
/* 自动续期只使已就绪凭据失效；不能解除永久错误或绕过正在进行的请求/退避。 */
bool device_handshake_renew(device_handshake_schedule_t *state);
bool device_handshake_due(const device_handshake_schedule_t *state, int64_t now_ms);
bool device_handshake_complete(device_handshake_schedule_t *state, uint32_t generation,
                               int status, bool success, int64_t now_ms);

#endif
