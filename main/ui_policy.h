/* 触屏手势与菜单命中规则。纯 C，无驱动依赖；生产界面和主机测试共用。
 * 手势规则参考 XiaoZhi VoCat TouchPolicy；MIT 许可见 docs/third-party/xiaozhi-LICENSE.txt。 */
#ifndef UI_POLICY_H
#define UI_POLICY_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { UI_GESTURE_NONE, UI_GESTURE_TAP, UI_GESTURE_UP, UI_GESTURE_DOWN } ui_gesture_t;
typedef struct { bool pressed; int start_x, start_y, last_x, last_y, travel; } ui_touch_policy_t;
typedef struct { ui_gesture_t gesture; int x, y; } ui_touch_result_t;
typedef enum { UI_PAGE_HOME, UI_PAGE_SETTINGS, UI_PAGE_CONFIRM, UI_PAGE_PROVISION } ui_page_t;
typedef enum {
    UI_ACTION_NONE, UI_ACTION_TALK, UI_ACTION_VOLUME_DOWN, UI_ACTION_VOLUME_UP,
    UI_ACTION_BRIGHTNESS_DOWN, UI_ACTION_BRIGHTNESS_UP, UI_ACTION_CONFIRM_WIFI,
    UI_ACTION_START_WIFI, UI_ACTION_STOP_WIFI, UI_ACTION_BACK,
} ui_action_t;
typedef struct { int x, y, width, height; ui_action_t action; } ui_button_t;

#define UI_BRIGHTNESS_DEFAULT 70
#define UI_BRIGHTNESS_MIN 10
#define UI_BRIGHTNESS_MAX 100

/* 背景和文字使用相同矩形，避免视觉框与实际点击区域错位。 */
static const ui_button_t ui_settings_buttons[] = {
    {44, 84, 56, 44, UI_ACTION_VOLUME_DOWN}, {260, 84, 56, 44, UI_ACTION_VOLUME_UP},
    {44, 140, 56, 44, UI_ACTION_BRIGHTNESS_DOWN}, {260, 140, 56, 44, UI_ACTION_BRIGHTNESS_UP},
    {58, 254, 150, 44, UI_ACTION_CONFIRM_WIFI}, {216, 254, 86, 44, UI_ACTION_BACK},
};
static const ui_button_t ui_confirm_buttons[] = {
    {66, 232, 110, 48, UI_ACTION_BACK}, {184, 232, 110, 48, UI_ACTION_START_WIFI},
};
static const ui_button_t ui_provision_buttons[] = {{100, 278, 160, 44, UI_ACTION_STOP_WIFI}};

static inline int ui_abs(int n) { return n < 0 ? -n : n; }
static inline void ui_touch_cancel(ui_touch_policy_t *p) { p->pressed = false; }
static inline ui_touch_result_t ui_touch_feed(ui_touch_policy_t *p, bool down, int x, int y)
{
    ui_touch_result_t result = {UI_GESTURE_NONE, 0, 0};
    if (down) {
        if (x < 0 || x >= 360 || y < 0 || y >= 360) {
            ui_touch_cancel(p);
            return result;
        }
        if (!p->pressed) {
            p->pressed = true; p->start_x = x; p->start_y = y; p->travel = 0;
        }
        p->last_x = x; p->last_y = y;
        int travel_x = ui_abs(x - p->start_x), travel_y = ui_abs(y - p->start_y);
        if (travel_x > p->travel) p->travel = travel_x;
        if (travel_y > p->travel) p->travel = travel_y;
        return result;
    }
    if (!p->pressed) return result;
    p->pressed = false;
    result.x = p->last_x; result.y = p->last_y;
    int dx = p->last_x - p->start_x, dy = p->last_y - p->start_y;
    if (p->start_y >= 300 && dy <= -60 && -dy > ui_abs(dx)) result.gesture = UI_GESTURE_UP;
    else if (dy >= 60 && dy > ui_abs(dx)) result.gesture = UI_GESTURE_DOWN;
    else if (p->travel <= 16) result.gesture = UI_GESTURE_TAP;
    return result;
}

static inline bool ui_button_contains(const ui_button_t *b, int x, int y)
{
    return x >= b->x && x < b->x + b->width && y >= b->y && y < b->y + b->height;
}
static inline ui_action_t ui_hit_test(ui_page_t page, int x, int y)
{
    const ui_button_t *buttons = NULL;
    unsigned count = 0;
    if (page == UI_PAGE_HOME) return UI_ACTION_TALK;
    if (page == UI_PAGE_SETTINGS) {
        buttons = ui_settings_buttons; count = sizeof(ui_settings_buttons) / sizeof(*buttons);
    } else if (page == UI_PAGE_CONFIRM) {
        buttons = ui_confirm_buttons; count = sizeof(ui_confirm_buttons) / sizeof(*buttons);
    } else if (page == UI_PAGE_PROVISION) {
        buttons = ui_provision_buttons; count = sizeof(ui_provision_buttons) / sizeof(*buttons);
    }
    for (unsigned i = 0; i < count; ++i)
        if (ui_button_contains(&buttons[i], x, y)) return buttons[i].action;
    return UI_ACTION_NONE;
}
static inline int ui_brightness_restore(int saved)
{
    return saved >= UI_BRIGHTNESS_MIN && saved <= UI_BRIGHTNESS_MAX ? saved : UI_BRIGHTNESS_DEFAULT;
}
static inline int ui_brightness_next(int current, bool increase)
{
    int next = ui_brightness_restore(current) + (increase ? 10 : -10);
    if (next < UI_BRIGHTNESS_MIN) next = UI_BRIGHTNESS_MIN;
    if (next > UI_BRIGHTNESS_MAX) next = UI_BRIGHTNESS_MAX;
    return next;
}
#endif
