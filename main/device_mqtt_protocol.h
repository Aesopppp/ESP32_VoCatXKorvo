/* 新平台 MQTT 的纯协议/策略。无网络和 RTOS 依赖，供设备与主机回归共用。 */
#ifndef DEVICE_MQTT_PROTOCOL_H
#define DEVICE_MQTT_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_TASK_PAYLOAD_MAX 4096u
#define DEVICE_TASK_ID_MAX 128u
#define DEVICE_TASK_HISTORY_SIZE 32u
#define DEVICE_TASK_QUEUE_SIZE 8u

typedef struct {
    char id[DEVICE_TASK_ID_MAX + 1];
    char payload[DEVICE_TASK_PAYLOAD_MAX + 1]; /* 原文；不重写 content 或 fire_at。 */
} device_mqtt_task_t;

bool device_mqtt_parse_task(const char *json, size_t length, device_mqtt_task_t *task);
/* UTC 时间只校验格式，不用本地时间丢弃迟到任务。 */

typedef struct {
    size_t total, used;
    char data[DEVICE_TASK_PAYLOAD_MAX + 1];
} device_mqtt_assembly_t;
typedef enum { DEVICE_MQTT_FRAGMENT_REJECTED, DEVICE_MQTT_FRAGMENT_MORE,
               DEVICE_MQTT_FRAGMENT_COMPLETE } device_mqtt_fragment_result_t;
/* first_allowed 由回调核对 topic/retain/QoS；后续片段可不携带 topic。
 * 不接受跳号、重叠、总长变化、NUL 或超长报文。一次 MQTT PUBLISH 不交错。 */
device_mqtt_fragment_result_t device_mqtt_assemble(device_mqtt_assembly_t *state,
    bool first_allowed, size_t total, size_t offset, const char *data, size_t length);

typedef struct {
    uint32_t version, next;
    char device_id[65];
    char ids[DEVICE_TASK_HISTORY_SIZE][DEVICE_TASK_ID_MAX + 1];
} device_mqtt_history_t;
void device_mqtt_history_init(device_mqtt_history_t *history, const char *device_id);
bool device_mqtt_history_valid(const device_mqtt_history_t *history, const char *device_id);
int device_mqtt_history_find(const device_mqtt_history_t *history, const char *id);
unsigned device_mqtt_history_add(device_mqtt_history_t *history, const char *id);

typedef struct {
    unsigned failures, auth_failures;
    int64_t retry_at_ms;
    bool blocked;
} device_mqtt_retry_t;
/* CONNACK 4/5 总计最多五次；SUBACK 拒绝不经过此处，保持连接并报错。 */
void device_mqtt_retry_failed(device_mqtt_retry_t *state, int connack, int64_t now_ms, uint32_t random);
void device_mqtt_retry_wake(device_mqtt_retry_t *state);
/* 0/1/2 是实际授予 QoS，128 或非法值返回 -1。 */
int device_mqtt_suback(const unsigned char *data, size_t length);
#endif
