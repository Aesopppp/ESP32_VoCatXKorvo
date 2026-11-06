/* 首页 EAF 动画。由现有 UI 任务调用，不读取音频数据、不改变会话状态。 */
#ifndef UI_FACE_H
#define UI_FACE_H
#include "lvgl.h"
#include "ui_service.h"
#include <stdint.h>

/* 启动阶段创建对象；首次 update 在 UI 任务内映射资源、申请固定 PSRAM 帧缓存。 */
void ui_face_init(lv_obj_t *parent);
/* 停止绘制后、LVGL 反初始化前调用；释放对象、缓冲和 Flash 映射。 */
void ui_face_deinit(void);
/* UI 任务传入单调时钟毫秒数。非首页隐藏并暂停刷新，状态变化立即更新。 */
void ui_face_update(ui_status_t status, bool visible, int64_t now_ms);
#endif
