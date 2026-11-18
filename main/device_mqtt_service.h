#ifndef DEVICE_MQTT_SERVICE_H
#define DEVICE_MQTT_SERVICE_H
#include "device_handshake.h"
#include "device_mqtt_protocol.h"
#include "esp_err.h"

/* 主任务初始化/轮询。worker 独占 MQTT 生命周期、任务队列和 NVS；
 * 回调仅复制有界事件，不调用音频、HTTP、Flash 或应用状态机。 */
esp_err_t device_mqtt_service_init(void);
void device_mqtt_service_poll(bool enabled, unsigned configuration_epoch,
                              const device_handshake_response_t *configuration);
/* 主任务单独更新空闲门禁；只在空闲时才把所有待播编号合成一个批次。 */
void device_mqtt_service_set_dispatch_allowed(bool allowed);
/* 仅真实用户唤醒解除 CONNACK 4/5 五次失败的熔断；任务/定时轮询不解除。 */
void device_mqtt_service_on_wake(void);
typedef struct {
    char device_id[65];
    unsigned count;
    char task_ids[DEVICE_TASK_QUEUE_SIZE][DEVICE_TASK_ID_MAX + 1];
} device_mqtt_delivery_t;
typedef enum {
    DEVICE_TASK_AUDIO_RECEIVED, /* 有有效音频、结束边界且本地输出排空。 */
    DEVICE_TASK_FAILED,         /* 失败回执，等待平台重新投递，不自行重播。 */
    DEVICE_TASK_NETWORK_WAIT,   /* 断网回执，保留任务至再次联网。 */
} device_mqtt_task_result_t;
/* 主任务仅在空闲、网络可用时批量领取；返回对象由服务拥有，必须恰好 finish 一次。
 * 成功先持久化去重记录，再回执；失败回执持久化至 broker PUBACK。 */
device_mqtt_delivery_t *device_mqtt_service_take_tasks(void);
void device_mqtt_service_finish_tasks(device_mqtt_delivery_t *tasks, device_mqtt_task_result_t result);
bool device_mqtt_service_voice_enabled(void);
#endif
