#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H
#include "esp_err.h"
#include "lvgl.h"

/* 初始化/失败回收只在启动阶段调用。成功后句柄由本模块持有至重启。
 * LVGL 的操作只允许 ui_service 的唯一任务执行（启动构建界面除外）。 */
esp_err_t display_service_init(void);
void display_service_deinit(void);
lv_display_t *display_service_get_display(void);
/* 主任务调用，仅修改 PWM，不操作 LVGL。10..100%；初始背光保持关闭。 */
esp_err_t display_service_set_brightness(int percent);
#endif
