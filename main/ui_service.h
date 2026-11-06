#ifndef UI_SERVICE_H
#define UI_SERVICE_H
#include "esp_err.h"
#include <stdbool.h>

/* 与业务状态机解耦的显示快照，可在未来增加平台握手状态，不携带任何密钥。
 * 全部字段按值复制；主任务不向绘图线程传递临时字符串指针。 */
typedef enum {
    UI_STATUS_STARTING, UI_STATUS_OFFLINE, UI_STATUS_WIFI_CONNECTING,
    UI_STATUS_WAIT_WAKE, UI_STATUS_CONNECTING, UI_STATUS_LISTENING,
    UI_STATUS_WAIT_REPLY, UI_STATUS_SPEAKING, UI_STATUS_CANCELLING,
    UI_STATUS_PROVISIONING, UI_STATUS_ERROR,
} ui_status_t;
typedef struct {
    ui_status_t status;
    int volume, brightness;
    bool wifi_connected, wifi_connecting, provisioning;
    char ssid[33];
    char ap_ssid[33];
    char provisioning_url[48];
} ui_snapshot_t;

/* 系统初始化末尾调用，硬件失败返回错误，现有语音/BOOT 仍继续运行。
 * 成功后创建一个 priority=2 的 UI 任务，唯一拥有 LVGL 和页面状态。 */
esp_err_t ui_service_init(void);
/* main_task 调用。容量 1 覆盖队列，复制最新快照，不阻塞业务。 */
void ui_service_update(const ui_snapshot_t *snapshot);
#endif
