#include "../main/ui_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ui_touch_policy_t p = {0};
    ui_touch_feed(&p, true, 180, 330);
    ui_touch_feed(&p, true, 181, 260);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_UP);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    /* 上滑入口/距离的临界值；斜滑不能误开菜单。 */
    ui_touch_feed(&p, true, 180, 299); ui_touch_feed(&p, true, 180, 239);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    ui_touch_feed(&p, true, 180, 300); ui_touch_feed(&p, true, 180, 241);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    ui_touch_feed(&p, true, 180, 300); ui_touch_feed(&p, true, 180, 240);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_UP);
    ui_touch_feed(&p, true, 180, 330); ui_touch_feed(&p, true, 250, 260);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    /* 中部上滑不打开菜单，也不会穿透为首页点击。 */
    ui_touch_feed(&p, true, 180, 250); ui_touch_feed(&p, true, 180, 170);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    /* 滑出再返回不是点击。 */
    ui_touch_feed(&p, true, 75, 100); ui_touch_feed(&p, true, 150, 100);
    ui_touch_feed(&p, true, 75, 100);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    ui_touch_feed(&p, true, 75, 100); ui_touch_feed(&p, true, 80, 105);
    ui_touch_result_t tap = ui_touch_feed(&p, false, 0, 0);
    assert(tap.gesture == UI_GESTURE_TAP);
    assert(ui_hit_test(UI_PAGE_SETTINGS, tap.x, tap.y) == UI_ACTION_VOLUME_DOWN);
    assert(ui_hit_test(UI_PAGE_CONFIRM, tap.x, tap.y) == UI_ACTION_NONE);
    assert(ui_hit_test(UI_PAGE_PROVISION, 180, 300) == UI_ACTION_STOP_WIFI);
    assert(ui_hit_test(UI_PAGE_SETTINGS, 180, 225) == UI_ACTION_NONE);
    assert(ui_hit_test(UI_PAGE_SETTINGS, 260, 140) == UI_ACTION_BRIGHTNESS_UP);
    assert(ui_hit_test(UI_PAGE_SETTINGS, 316, 140) == UI_ACTION_NONE);
    ui_touch_feed(&p, true, 180, 150); ui_touch_feed(&p, true, 180, 210);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_DOWN);
    ui_touch_feed(&p, true, 180, 150); ui_touch_feed(&p, true, 196, 150);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_TAP);
    ui_touch_feed(&p, true, 180, 150); ui_touch_feed(&p, true, 197, 150);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    ui_touch_feed(&p, true, 180, 200); ui_touch_cancel(&p);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    /* 越界坐标丢弃整个手势。 */
    ui_touch_feed(&p, true, 180, 200); ui_touch_feed(&p, true, 360, 200);
    assert(ui_touch_feed(&p, false, 0, 0).gesture == UI_GESTURE_NONE);
    assert(ui_brightness_restore(-1) == 70);
    assert(ui_brightness_restore(0) == 70);
    assert(ui_brightness_restore(10) == 10);
    assert(ui_brightness_restore(100) == 100);
    assert(ui_brightness_restore(101) == 70);
    assert(ui_brightness_next(10, false) == 10);
    assert(ui_brightness_next(100, true) == 100);
    assert(ui_brightness_next(50, true) == 60);
    puts("UI policy checks passed: gestures, isolation, bounds, brightness restore/clamp");
    return 0;
}
