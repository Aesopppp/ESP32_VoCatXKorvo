/* VoCat 首页、设置及配网页面。UI 只发操作事件，不直接控制网络或音频。 */
#include "ui_service.h"
#include "ui_policy.h"
#include "ui_face.h"
#include "display_service.h"
#include "touch_service.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_service";
LV_FONT_DECLARE(vocat_ui_font_20);
static lv_font_t s_font;
static QueueHandle_t s_updates;
static TaskHandle_t s_task;
static ui_snapshot_t s_state = {.status = UI_STATUS_STARTING};
static ui_page_t s_page;
static lv_obj_t *s_boxes[16], *s_labels[16];
static lv_obj_t *s_home_layer, *s_menu_layer;
static lv_obj_t *s_home_boxes[2], *s_home_labels[2];
static bool s_dirty;
static int64_t s_pending_until_ms;
static const char *s_notice;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#define MENU_HEIGHT 360
#define MENU_SLIDE_MS 250
#define MENU_FRAME_MS 50 /* 与表情相同的 20fps 节拍；忙时跳帧，不追赶补帧。 */
static bool s_menu_sliding;
static int64_t s_slide_started_ms, s_slide_next_ms;

static void set_layer_visible(lv_obj_t *layer, bool visible)
{
    if (visible) lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
}

/* 仅移动菜单容器，文字/按钮共用父坐标；不改屏幕扫描方向或触摸坐标。
 * 过渡时保留首页作为背景，并暂停表情解码；同一 UI 任务合成后整帧提交。 */
static void update_menu_slide(int64_t time_ms)
{
    if (!s_menu_sliding || time_ms < s_slide_next_ms) return;
    int64_t elapsed = time_ms - s_slide_started_ms;
    if (elapsed >= MENU_SLIDE_MS) {
        s_menu_sliding = false;
        lv_obj_set_y(s_menu_layer, s_page == UI_PAGE_HOME ? MENU_HEIGHT : 0);
        set_layer_visible(s_menu_layer, s_page != UI_PAGE_HOME);
        set_layer_visible(s_home_layer, s_page == UI_PAGE_HOME);
        /* 作废过渡中开始的触摸及排队手势，避免动画结束时误点到首页/按钮。 */
        touch_service_cancel();
        s_dirty = true;
        return;
    }
    int64_t remaining = MENU_SLIDE_MS - elapsed;
    int offset = (int)(MENU_HEIGHT * remaining * remaining / (MENU_SLIDE_MS * MENU_SLIDE_MS));
    lv_obj_set_y(s_menu_layer, s_page == UI_PAGE_HOME ? MENU_HEIGHT - offset : offset);
    s_slide_next_ms = s_slide_started_ms + (elapsed / MENU_FRAME_MS + 1) * MENU_FRAME_MS;
}

static const char *status_text(ui_status_t status)
{
    switch (status) {
    case UI_STATUS_STARTING: return "正在启动";
    case UI_STATUS_OFFLINE: return "网络未连接";
    case UI_STATUS_WIFI_CONNECTING: return "正在连接网络";
    case UI_STATUS_CONNECTING: return "正在连接平台";
    case UI_STATUS_WAIT_WAKE:
    case UI_STATUS_LISTENING:
    case UI_STATUS_WAIT_REPLY:
    case UI_STATUS_SPEAKING:
    case UI_STATUS_CANCELLING: return NULL; /* 常规会话状态由表情动画表示。 */
    case UI_STATUS_PROVISIONING: return "设备配网";
    default: return "连接异常";
    }
}

/* 每个按钮使用独立背景，文字在其内部居中；布局矩形来自 ui_policy.h。 */
static void item(unsigned index, int x, int y, int width, int height, const char *text, bool button)
{
    lv_obj_t *box = s_page == UI_PAGE_HOME ? s_home_boxes[index] : s_boxes[index];
    lv_obj_t *label = s_page == UI_PAGE_HOME ? s_home_labels[index] : s_labels[index];
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(box, x, y); lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_opa(box, button ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x20363d), 0);
    lv_obj_set_style_border_width(box, button ? 1 : 0, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x38545c), 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_width(label, width - 8);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

static void button(unsigned index, const ui_button_t *rect, const char *text)
{
    item(index, rect->x, rect->y, rect->width, rect->height, text, true);
}

static void render(void)
{
    /* 首页和菜单对象分开保留：菜单滑出时不能提前清空仍可见的文字/按钮。 */
    if (s_page == UI_PAGE_HOME) {
        for (unsigned i = 0; i < 2; ++i) lv_obj_add_flag(s_home_boxes[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        for (unsigned i = 0; i < 16; ++i) lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }
    char volume[40], brightness[40];
    snprintf(volume, sizeof(volume), "音量  %d", s_state.volume);
    snprintf(brightness, sizeof(brightness), "亮度  %d%%", s_state.brightness);
    switch (s_page) {
    case UI_PAGE_HOME:
        if (status_text(s_state.status)) item(0, 50, 52, 260, 36, status_text(s_state.status), false);
        if (s_notice) item(1, 45, 288, 270, 32, s_notice, false);
        break;
    case UI_PAGE_SETTINGS:
        item(0, 80, 36, 200, 32, "设备设置", false);
        button(1, &ui_settings_buttons[0], "-"); button(2, &ui_settings_buttons[1], "+");
        item(3, 105, 84, 150, 44, volume, false);
        button(4, &ui_settings_buttons[2], "-"); button(5, &ui_settings_buttons[3], "+");
        item(6, 105, 140, 150, 44, brightness, false);
        item(7, 80, 193, 200, 26, "当前网络", false);
        item(8, 52, 219, 256, 28, s_state.wifi_connected ? s_state.ssid :
             s_state.wifi_connecting ? "连接中" : "未连接", false);
        lv_label_set_long_mode(s_labels[8], LV_LABEL_LONG_SCROLL_CIRCULAR);
        button(9, &ui_settings_buttons[4], "进入配网"); button(10, &ui_settings_buttons[5], "返回");
        if (s_notice) item(11, 80, 307, 200, 26, s_notice, false);
        break;
    case UI_PAGE_CONFIRM:
        item(0, 70, 46, 220, 36, "进入配网", false);
        item(1, 35, 120, 290, 76, "将结束当前对话\n保留设备和网络配置", false);
        button(2, &ui_confirm_buttons[0], "返回"); button(3, &ui_confirm_buttons[1], "确认");
        item(4, 70, 295, 220, 28, s_notice ? s_notice : "", false);
        break;
    case UI_PAGE_PROVISION:
        item(0, 70, 40, 220, 32, "设备配网", false);
        item(1, 35, 92, 290, 28, "手机连接以下热点", false);
        item(2, 35, 128, 290, 32, s_state.ap_ssid, false);
        item(3, 65, 167, 230, 28, "无需密码", false);
        item(4, 30, 205, 300, 26, "连接后打开网页", false);
        item(5, 35, 237, 290, 28, s_state.provisioning_url, false);
        button(6, &ui_provision_buttons[0], s_notice ? s_notice : "退出配网");
        break;
    }
    /* 滑入时保留原表情作背景；滑出前恢复首页，随后冻结到过渡结束。 */
    if (!s_menu_sliding) ui_face_update(s_state.status, s_page == UI_PAGE_HOME, now_ms());
    s_dirty = false;
}

static void set_page(ui_page_t page)
{
    if (page == s_page) return;
    touch_service_cancel();
    ui_page_t previous = s_page;
    bool slide_in = previous == UI_PAGE_HOME && page == UI_PAGE_SETTINGS;
    bool slide_out = page == UI_PAGE_HOME &&
                     (previous == UI_PAGE_SETTINGS || previous == UI_PAGE_CONFIRM);
    s_page = page; s_notice = NULL; s_pending_until_ms = 0; s_dirty = true;
    /* 配网状态来自业务快照，应立即显示；可安全覆盖未完成的菜单动画。 */
    s_menu_sliding = slide_in || slide_out;
    if (s_menu_sliding) {
        set_layer_visible(s_home_layer, true);
        set_layer_visible(s_menu_layer, true);
        lv_obj_set_y(s_menu_layer, slide_in ? MENU_HEIGHT : 0);
        if (slide_out) ui_face_update(s_state.status, true, now_ms());
        s_slide_started_ms = now_ms();
        s_slide_next_ms = s_slide_started_ms;
    } else {
        lv_obj_set_y(s_menu_layer, 0);
        set_layer_visible(s_home_layer, page == UI_PAGE_HOME);
        set_layer_visible(s_menu_layer, page != UI_PAGE_HOME);
    }
}

static void post_action(event_type_t event, bool wait_for_page)
{
    if (event_bus_publish(event, NULL, 0) != pdPASS) {
        s_notice = "操作繁忙，请重试"; s_dirty = true;
        return;
    }
    s_notice = NULL; s_dirty = true;
    if (wait_for_page) {
        s_pending_until_ms = now_ms() + 8000;
        s_notice = "正在处理"; s_dirty = true;
    }
}

static void handle_touch(ui_touch_result_t touch)
{
    if (touch.gesture == UI_GESTURE_NONE || s_pending_until_ms || s_menu_sliding) return;
    if (touch.gesture == UI_GESTURE_UP && s_page == UI_PAGE_HOME) {
        set_page(UI_PAGE_SETTINGS); return;
    }
    if (touch.gesture == UI_GESTURE_DOWN &&
        (s_page == UI_PAGE_SETTINGS || s_page == UI_PAGE_CONFIRM)) {
        set_page(UI_PAGE_HOME); return;
    }
    if (touch.gesture != UI_GESTURE_TAP) return;
    switch (ui_hit_test(s_page, touch.x, touch.y)) {
    case UI_ACTION_TALK: post_action(EV_UI_TALK, false); break;
    case UI_ACTION_VOLUME_DOWN: post_action(EV_BUTTON_VOLUME_DOWN, false); break;
    case UI_ACTION_VOLUME_UP: post_action(EV_BUTTON_VOLUME_UP, false); break;
    case UI_ACTION_BRIGHTNESS_DOWN: post_action(EV_UI_BRIGHTNESS_DOWN, false); break;
    case UI_ACTION_BRIGHTNESS_UP: post_action(EV_UI_BRIGHTNESS_UP, false); break;
    case UI_ACTION_CONFIRM_WIFI: set_page(UI_PAGE_CONFIRM); break;
    case UI_ACTION_START_WIFI: post_action(EV_UI_PROVISION, true); break;
    case UI_ACTION_STOP_WIFI: post_action(EV_UI_PROVISION_EXIT, true); break;
    case UI_ACTION_BACK: set_page(s_page == UI_PAGE_CONFIRM ? UI_PAGE_SETTINGS : UI_PAGE_HOME); break;
    default: break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        ui_snapshot_t snapshot;
        if (xQueueReceive(s_updates, &snapshot, 0) == pdTRUE && memcmp(&s_state, &snapshot, sizeof(snapshot))) {
            bool was_provisioning = s_state.provisioning;
            s_state = snapshot;
            if (snapshot.provisioning) set_page(UI_PAGE_PROVISION);
            else if (was_provisioning) set_page(UI_PAGE_HOME);
            s_dirty = true;
        }
        if (s_pending_until_ms && now_ms() >= s_pending_until_ms) {
            s_pending_until_ms = 0; s_notice = "请重试"; s_dirty = true;
        }
        handle_touch(touch_service_poll());
        update_menu_slide(now_ms());
        if (s_dirty) render();
        else if (!s_menu_sliding) ui_face_update(s_state.status, s_page == UI_PAGE_HOME, now_ms());
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10)); /* 50 ms 动画节拍的检查精度；DMA 等待仍让出 CPU。 */
    }
}

static lv_obj_t *create_page_layer(lv_obj_t *screen, uint32_t color)
{
    lv_obj_t *layer = lv_obj_create(screen);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, 360, MENU_HEIGHT);
    lv_obj_set_style_bg_color(layer, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return layer;
}

static void create_items(lv_obj_t *parent, lv_obj_t **boxes, lv_obj_t **labels, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        boxes[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(boxes[i]);
        lv_obj_remove_flag(boxes[i], LV_OBJ_FLAG_SCROLLABLE);
        labels[i] = lv_label_create(boxes[i]);
        lv_obj_set_style_text_font(labels[i], &s_font, 0);
        lv_obj_set_style_text_color(labels[i], lv_color_hex(0xe7f4ef), 0);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
    }
}

esp_err_t ui_service_init(void)
{
    if (s_task) return ESP_OK;
    esp_err_t err = display_service_init();
    if (err != ESP_OK) return err;
    s_updates = xQueueCreate(1, sizeof(ui_snapshot_t));
    if (!s_updates) { display_service_deinit(); return ESP_ERR_NO_MEM; }
    err = touch_service_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "Touch unavailable: %s; BOOT remains available", esp_err_to_name(err));
    s_font = vocat_ui_font_20;
    /* 常见中文 SSID 使用 LVGL 自带字库补充，菜单文字使用完整专用子集。 */
    s_font.fallback = &lv_font_source_han_sans_sc_16_cjk;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_home_layer = create_page_layer(screen, 0x000000);
    ui_face_init(s_home_layer);
    create_items(s_home_layer, s_home_boxes, s_home_labels, 2);
    s_menu_layer = create_page_layer(screen, 0x0c171d);
    create_items(s_menu_layer, s_boxes, s_labels, 16);
    set_layer_visible(s_menu_layer, false);
    /* 默认 40 ms 刷新周期与 50 ms 帧节拍错位；10 ms 检查一次，有变化才整帧重绘。 */
    lv_timer_set_period(lv_display_get_refr_timer(display_service_get_display()), 10);
    /* 首帧也由 UI 任务绘制，避免在 IDF 启动任务的小栈上执行完整渲染。
     * 任务创建失败时尚无 DMA 在途，启动资源可以同步安全回收。 */
    s_dirty = true;
    if (xTaskCreatePinnedToCore(ui_task, "vocat_ui", 6144, NULL, 2, &s_task, 0) != pdPASS) {
        ui_face_deinit(); touch_service_deinit(); display_service_deinit();
        vQueueDelete(s_updates); s_updates = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI ready: bottom swipe=settings, tap=wake/interrupt");
    return ESP_OK;
}

void ui_service_update(const ui_snapshot_t *snapshot)
{
    if (s_updates && snapshot) xQueueOverwrite(s_updates, snapshot);
}
