/* EAF 表情与现有 LVGL 共用 UI 任务和唯一 LCD flush，不创建另一个图形引擎。
 * 每 50 ms 最多解码当前一帧，忙时跳帧，不补播；菜单隐藏期间不解码。
 * 动画表达设备状态，不读取音频、不改变会话，不推断开心/难过等语义。 */
#include "ui_face.h"
#include "ui_face_assets.h"
#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <string.h>

#define FACE_FRAME_MS 50
#define EYE_WIDTH 125
#define EYE_HEIGHT 160
#define EYE_PIXELS (EYE_WIDTH * EYE_HEIGHT)
#define INDICATOR_WIDTH 100
#define INDICATOR_HEIGHT 46
#define INDICATOR_PIXELS (INDICATOR_WIDTH * INDICATOR_HEIGHT)

static const char *TAG = "ui_face";
static lv_obj_t *s_eyes[2], *s_indicator, *s_fallback[2];
static lv_image_dsc_t s_images[3];
static uint16_t *s_pixels; /* 89,200 bytes：双眼 + 状态条，来自 LVGL 的 PSRAM 分配器。 */
static ui_animation_t s_animations[UI_ANIM_COUNT];
static bool s_attempted, s_ready, s_visible;
static ui_status_t s_status;
static int64_t s_started_ms, s_next_frame_ms;
static int s_eye_animation = -1, s_eye_frame = -1, s_indicator_frame = -1;

static void set_visible(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *new_object(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    return object;
}

static void prepare_resources(void)
{
    s_attempted = true;
    esp_err_t err = ui_face_assets_open(s_animations);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Animation assets unavailable (%s); static eyes retained. Flash ui_assets.bin too",
                 esp_err_to_name(err));
        return;
    }
    s_pixels = lv_malloc((2 * EYE_PIXELS + INDICATOR_PIXELS) * sizeof(uint16_t));
    if (!s_pixels) {
        ui_face_assets_close();
        ESP_LOGW(TAG, "Animation PSRAM unavailable; static eyes retained");
        return;
    }
    memset(s_pixels, 0, (2 * EYE_PIXELS + INDICATOR_PIXELS) * sizeof(uint16_t));
    for (unsigned i = 0; i < 3; ++i) {
        unsigned width = i == 2 ? INDICATOR_WIDTH : EYE_WIDTH;
        unsigned height = i == 2 ? INDICATOR_HEIGHT : EYE_HEIGHT;
        s_images[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = width, .h = height, .stride = width * sizeof(uint16_t)},
            .data_size = width * height * sizeof(uint16_t),
            .data = (const uint8_t *)(s_pixels + i * EYE_PIXELS),
        };
        lv_image_set_src(i == 2 ? s_indicator : s_eyes[i], &s_images[i]);
    }
    s_ready = true;
    ESP_LOGI(TAG, "EAF ready: 20 fps, fixed PSRAM=%u bytes, frames=%u/%u/%u/%u",
             (unsigned)((2 * EYE_PIXELS + INDICATOR_PIXELS) * sizeof(uint16_t)),
             s_animations[0].frame_count, s_animations[1].frame_count,
             s_animations[2].frame_count, s_animations[3].frame_count);
}

void ui_face_init(lv_obj_t *parent)
{
    /* 与旧 VoCat 布局一致：单眼 125x160，左右镜像，中心纵向偏移 10 px。 */
    for (unsigned i = 0; i < 2; ++i) {
        s_eyes[i] = lv_image_create(parent);
        lv_obj_set_pos(s_eyes[i], i ? 225 : 10, 110);
        lv_obj_remove_flag(s_eyes[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_eyes[i], LV_OBJ_FLAG_HIDDEN);
        s_fallback[i] = new_object(parent, i ? 225 : 50, 145, 85, 85);
        lv_obj_set_style_radius(s_fallback[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_fallback[i], lv_color_hex(0xe0f4ff), 0);
        lv_obj_set_style_bg_opa(s_fallback[i], LV_OPA_COVER, 0);
    }
    s_indicator = lv_image_create(parent);
    lv_obj_set_pos(s_indicator, 130, 25);
    lv_obj_remove_flag(s_indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_indicator, LV_OBJ_FLAG_HIDDEN);
}

void ui_face_deinit(void)
{
    /* 调用前 UI 已停止刷新/没有在途绘制。启动失败路径也可调用。 */
    for (unsigned i = 0; i < 3; ++i) lv_image_cache_drop(&s_images[i]);
    for (unsigned i = 0; i < 2; ++i) {
        if (s_eyes[i]) lv_obj_delete(s_eyes[i]);
        if (s_fallback[i]) lv_obj_delete(s_fallback[i]);
        s_eyes[i] = s_fallback[i] = NULL;
    }
    if (s_indicator) lv_obj_delete(s_indicator);
    s_indicator = NULL;
    lv_free(s_pixels); s_pixels = NULL;
    ui_face_assets_close();
    s_attempted = s_ready = s_visible = false;
    s_eye_animation = s_eye_frame = s_indicator_frame = -1;
}

static bool update_eye(ui_animation_id_t id, unsigned frame)
{
    if (s_eye_animation == (int)id && s_eye_frame == (int)frame) return true;
    if (!ui_animation_decode(&s_animations[id], frame, s_pixels, EYE_PIXELS, 0)) return false;
    uint16_t *right = s_pixels + EYE_PIXELS;
    for (unsigned y = 0; y < EYE_HEIGHT; ++y) {
        for (unsigned x = 0; x < EYE_WIDTH; ++x)
            right[y * EYE_WIDTH + x] = s_pixels[y * EYE_WIDTH + EYE_WIDTH - x - 1];
    }
    for (unsigned i = 0; i < 2; ++i) {
        lv_image_cache_drop(&s_images[i]);
        lv_obj_invalidate(s_eyes[i]);
    }
    s_eye_animation = id; s_eye_frame = (int)frame;
    return true;
}

void ui_face_update(ui_status_t status, bool visible, int64_t now_ms)
{
    if (!s_eyes[0]) return;
    /* 取消是业务内部的短暂状态，收到快照就显示监听，不播放单独的过渡画面。
     * 在比较状态前归一化，使 cancelling -> listening 继续同一动画时间轴，避免重启首帧。
     * 这里只改变显示；旧回答取消确认和句首音频保护仍由业务状态机负责。 */
    if (status == UI_STATUS_CANCELLING) status = UI_STATUS_LISTENING;
    if (!visible) {
        for (unsigned i = 0; i < 2; ++i) {
            set_visible(s_eyes[i], false); set_visible(s_fallback[i], false);
        }
        set_visible(s_indicator, false);
        s_visible = false;
        return;
    }
    if (!s_attempted) prepare_resources();
    bool changed = !s_visible || status != s_status;
    if (changed) {
        s_status = status; s_started_ms = now_ms; s_visible = true;
    } else if (now_ms < s_next_frame_ms) return;
    unsigned tick = (unsigned)((now_ms - s_started_ms) / FACE_FRAME_MS);
    /* 按时间轴选择下一帧，不把解码/刷新耗时累加成播放变慢。 */
    s_next_frame_ms = s_started_ms + ((int64_t)tick + 1) * FACE_FRAME_MS;
    bool indicator = status == UI_STATUS_LISTENING || status == UI_STATUS_SPEAKING;
    if (s_ready) {
        ui_animation_id_t id = status == UI_STATUS_WAIT_REPLY ? UI_ANIM_THINKING : UI_ANIM_NEUTRAL;
        unsigned frame = tick % s_animations[id].frame_count;
        if (status != UI_STATUS_WAIT_WAKE && status != UI_STATUS_WAIT_REPLY && !indicator) frame = 0;
        s_ready = update_eye(id, frame);
        if (s_ready && indicator) {
            unsigned index = tick % s_animations[UI_ANIM_LISTEN].frame_count;
            if (s_indicator_frame != (int)index) {
                s_ready = ui_animation_decode(&s_animations[UI_ANIM_LISTEN], index,
                    s_pixels + 2 * EYE_PIXELS, INDICATOR_PIXELS, 0);
                lv_image_cache_drop(&s_images[2]);
                lv_obj_invalidate(s_indicator);
                s_indicator_frame = (int)index;
            }
        }
        if (!s_ready) ESP_LOGW(TAG, "Animation decode failed; static eyes retained");
    }
    for (unsigned i = 0; i < 2; ++i) {
        set_visible(s_eyes[i], s_ready);
        set_visible(s_fallback[i], !s_ready);
    }
    if (changed) lv_obj_set_y(s_indicator, status == UI_STATUS_SPEAKING ? 284 : 25);
    set_visible(s_indicator, s_ready && indicator);
}
