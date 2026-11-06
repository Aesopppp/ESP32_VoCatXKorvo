#ifndef TOUCH_SERVICE_H
#define TOUCH_SERVICE_H
#include "esp_err.h"
#include "ui_policy.h"
/* init/deinit 在启动阶段调用；采样任务借用音频的 I2C 总线，不删除总线。
 * deinit 等待当前有界 I2C 读操作退出，不可与 poll/cancel 并发调用。
 * poll/cancel 仅由 UI 任务调用：非阻塞收取手势/取消旧页面手势。 */
esp_err_t touch_service_init(void);
void touch_service_deinit(void);
ui_touch_result_t touch_service_poll(void);
void touch_service_cancel(void);
#endif
