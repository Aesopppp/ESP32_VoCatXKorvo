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
/* 仅真实用户唤醒解除 CONNACK 4/5 五次失败的熔断；任务/定时轮询不解除。 */
void device_mqtt_service_on_wake(void);
typedef struct {
    device_mqtt_task_t message;
    char device_id[65];
} device_mqtt_delivery_t;
/* 主任务仅在空闲、网络可用时领取；返回对象由服务拥有，必须恰好 finish 一次。
 * false 结束不会记已播报或 ACK。调用后指针立即失效，不能自行 free。 */
device_mqtt_delivery_t *device_mqtt_service_take_task(void);
void device_mqtt_service_finish_task(device_mqtt_delivery_t *task, bool played);
bool device_mqtt_service_voice_enabled(void);
#endif
