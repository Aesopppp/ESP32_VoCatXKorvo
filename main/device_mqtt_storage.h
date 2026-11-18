/* MQTT 任务的持久化格式。独立于 NVS/RTOS，兼容旧固定结构体记录。 */
#ifndef DEVICE_MQTT_STORAGE_H
#define DEVICE_MQTT_STORAGE_H
#include "device_mqtt_protocol.h"

#define MQTT_PENDING_VERSION 1u
enum { TASK_READY, TASK_WAIT_PLATFORM, TASK_WAIT_NETWORK };
typedef struct {
    char id[DEVICE_TASK_ID_MAX + 1];
    int64_t received_at;
    uint8_t state, failure_pending;
} mqtt_pending_entry_t;

typedef struct {
    uint32_t version, count;
    char device_id[65];
    mqtt_pending_entry_t entries[DEVICE_TASK_QUEUE_SIZE];
} mqtt_pending_store_t;

/* 同时容纳旧结构体及新格式；仅 worker 临时使用，不放在任务栈上。 */
#define DEVICE_MQTT_STORAGE_MAX (sizeof(device_mqtt_history_t) + sizeof(mqtt_pending_store_t))

/* 返回实际字节数，0 表示参数/容量错误。ID 始终作为字符串，无整数转换。 */
size_t device_mqtt_storage_encode_history(const device_mqtt_history_t *history, void *data, size_t capacity);
size_t device_mqtt_storage_encode_pending(const mqtt_pending_store_t *pending, void *data, size_t capacity);
/* 只检查存储结构；调用方继续校验身份、ID 和业务状态。legacy 用于延后迁移。 */
bool device_mqtt_storage_decode_history(const void *data, size_t size, device_mqtt_history_t *history, bool *legacy);
bool device_mqtt_storage_decode_pending(const void *data, size_t size, mqtt_pending_store_t *pending, bool *legacy);
#endif
