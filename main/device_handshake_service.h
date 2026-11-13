#ifndef DEVICE_HANDSHAKE_SERVICE_H
#define DEVICE_HANDSHAKE_SERVICE_H

#include "device_handshake.h"
#include "esp_err.h"

/* NVS、wifi_service 初始化后，由 main_task 初始化一次。
 * 接口均由 main_task 串行调用；HTTP I/O 在独立低优先级 worker 内完成。 */
esp_err_t device_handshake_service_init(void);

/* 非阻塞；network_available 应同时考虑 GOT_IP 和是否处于配网模式。 */
void device_handshake_service_poll(bool network_available);

/* 显式重读整组配置并解除永久错误；不能在每次 Wi-Fi 重连时调用。
 * 只读 NVS、不写 Flash。供未来安全注入/人工更新入口使用。 */
esp_err_t device_handshake_service_refresh(void);

/* 返回只读 RAM 快照，未成功/断网时为 NULL；借用至下一次 poll/refresh。
 * 新建语音连接必须先调用 prepare_websocket 检查 TTL，并由 WS 服务复制凭据。 */
const device_handshake_response_t *device_handshake_service_get_configuration(void);

typedef enum {
    DEVICE_WS_DISABLED, /* 仅显式 enabled:false 允许沿用旧语音配置。 */
    DEVICE_WS_WAITING,
    DEVICE_WS_READY,
    DEVICE_WS_BLOCKED,
} device_ws_readiness_t;

/* 主任务非阻塞入口：接近过期时请求一次续期；调用者继续 poll，不能忙等。
 * READY 时 configuration 借用到下一次服务调用；正在通话时不调用此入口。 */
device_ws_readiness_t device_handshake_service_prepare_websocket(
    const device_handshake_response_t **configuration);
bool device_handshake_service_is_enabled(void);
bool device_handshake_service_is_blocked(void);
/* 鉴权拒绝后由主任务有限次数调用；不重读身份、不解除 HTTP 永久错误。 */
bool device_handshake_service_renew_credentials(void);

/* 主任务访问：显式重读身份/网络代次变化时递增，单纯 WS token 续期不变。
 * MQTT 可保留自己的凭据副本，但不能跨身份或网络代次继续使用旧配置。 */
unsigned device_handshake_service_mqtt_epoch(void);

#endif
