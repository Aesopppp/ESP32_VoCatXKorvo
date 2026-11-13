#ifndef DEVICE_HANDSHAKE_HTTP_H
#define DEVICE_HANDSHAKE_HTTP_H

#include "device_handshake.h"

typedef struct { const char *name, *value; } device_handshake_header_t;

/* 窄传输接口：生产适配 esp_http_client，主机测试注入短读/超时/断网。
 * begin 必须禁用重定向、验证 HTTPS 证书；字符串仅借用至 close，不保存到任务外。
 * read 返回字节数，0 表示没有更多数据，负值表示失败；complete 区分 EOF 与截断。
 * 所有回调都在后台 worker 内串行调用；close 在每个退出路径都会执行。 */
typedef struct {
    void *context;
    bool (*begin)(void *context, const char *url, const device_handshake_header_t *headers,
                  size_t header_count, const char *body, int *status, int64_t *content_length);
    int (*read)(void *context, char *buffer, size_t size);
    bool (*complete)(void *context);
    void (*close)(void *context);
    bool (*cancelled)(void *context);
    int64_t (*now_ms)(void *context);
} device_handshake_http_t;

typedef struct {
    int status;       /* 0=网络/资源/读取失败；已收到的永久 HTTP 错误不会被降级为 0。 */
    bool success;
    const char *reason; /* 固定字符串，可安全输出日志。 */
    device_handshake_response_t response;
} device_handshake_result_t;

/* config 必须通过 parse_config；output 由调用方分配，不能放在小任务栈上。 */
void device_handshake_exchange(const device_handshake_http_t *http,
                               const device_handshake_config_t *config,
                               const char *version, const char *mac,
                               device_handshake_result_t *output);

#endif
