#ifndef UI_FACE_ASSETS_H
#define UI_FACE_ASSETS_H
#include "esp_err.h"
#include "ui_animation.h"

/* 仅由 UI 任务首次播放时调用。只读映射 ui_assets，校验一次；不下载、不写 Flash。
 * 成功后动画借用映射直到 close；必须先停止绘制再关闭。重复 open 返回同一资源。 */
esp_err_t ui_face_assets_open(ui_animation_t animations[UI_ANIM_COUNT]);
void ui_face_assets_close(void);
#endif
