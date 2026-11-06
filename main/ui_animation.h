/* 固定资源的 EAF/RLE 解码器。纯 C，无任务、堆分配、LVGL 或硬件依赖。 */
#ifndef UI_ANIMATION_H
#define UI_ANIMATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 与 assets/face/manifest.json 的顺序一致；资源包版本变化时须显式迁移。 */
typedef enum {
    UI_ANIM_NEUTRAL, UI_ANIM_WINKING, UI_ANIM_THINKING, UI_ANIM_LISTEN,
    UI_ANIM_COUNT,
} ui_animation_id_t;

typedef struct {
    const uint8_t *data; /* 借用只读映射；整个播放期必须有效。 */
    size_t size;
    uint16_t width, height, frame_count;
    uint16_t table_count; /* 包含末尾 _C 元信息帧；该帧不播放。 */
} ui_animation_t;

/* 校验完整资源包及每帧边界/编码。失败后调用者不能使用输出；不修改输入。 */
bool ui_animation_pack_open(const void *data, size_t size, ui_animation_t out[UI_ANIM_COUNT]);
bool ui_animation_open(const void *data, size_t size, ui_animation_t *out);
/* 输出本机字节序 RGB565，透明色填 background。容量单位为像素。
 * 只支持当前固定资源使用的 8 位调色板 + RLE；其他格式明确拒绝。
 * 失败时输出可能部分更新，调用者应隐藏该图片，不能呈现不完整帧。 */
bool ui_animation_decode(const ui_animation_t *animation, unsigned frame,
                         uint16_t *pixels, size_t capacity, uint16_t background);
#endif
