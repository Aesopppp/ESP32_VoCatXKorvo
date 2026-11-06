/* 桌面执行真实 ui_service.c/LVGL；仅替换 RTOS、触摸和 LCD 传输。
 * 用于页面渲染/动作路由回归，不能证明 DMA、电气或实板触控正确。 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../main/ui_face_assets.h"
#include "../../main/ui_service.c"
static lv_display_t *mock_display;
static uint16_t frame[360*360], draw_buffer[360*360];
static unsigned frame_submissions;
static event_type_t last_event;
static unsigned event_count;
static unsigned touch_cancel_count;
static int64_t mock_time_ms = 1000;
static bool missing_assets;
static uint8_t *asset_data;
static unsigned asset_open_count;
esp_err_t ui_face_assets_open(ui_animation_t animations[UI_ANIM_COUNT]) {
 ++asset_open_count;
 if(missing_assets) return ESP_ERR_NO_MEM;
 FILE *f=fopen("work/ui-checks/ui_assets.bin","rb"); assert(f);
 fseek(f,0,SEEK_END); size_t size=(size_t)ftell(f); rewind(f);
 asset_data=malloc(size); assert(asset_data);
 assert(fread(asset_data,1,size,f)==size); fclose(f);
 assert(ui_animation_pack_open(asset_data,size,animations)); return ESP_OK;
}
void ui_face_assets_close(void) { free(asset_data); asset_data=NULL; }
int64_t esp_timer_get_time(void) { return mock_time_ms * 1000; }
QueueHandle_t xQueueCreate(unsigned n,unsigned sz) { return (void*)1; }
int xQueueReceive(QueueHandle_t q,void*p,unsigned t) { return 0; }
int xQueueOverwrite(QueueHandle_t q,const void*p) { return 1; }
void vQueueDelete(QueueHandle_t q) {}
int xTaskCreatePinnedToCore(void (*f)(void*),const char*n,unsigned st,void*a,unsigned pr,TaskHandle_t*t,int c) { *t=(void*)1; return 1; }
void vTaskDelay(unsigned t) {}
BaseType_t event_bus_publish(event_type_t e,const void*p,size_t n) { last_event=e; ++event_count; return 1; }
esp_err_t touch_service_init(void) { return 0; }
void touch_service_deinit(void) {}
void touch_service_cancel(void) { ++touch_cancel_count; }
ui_touch_result_t touch_service_poll(void) { return (ui_touch_result_t){0}; }
static void host_flush(lv_display_t *d,const lv_area_t *a,uint8_t *map) {
 /* 此替身只检查 LVGL 每次交付完整画面；实际分段传输另由 display_checks.c 验证。 */
 assert(a->x1==0 && a->y1==0 && a->x2==359 && a->y2==359);
 assert(lv_display_flush_is_last(d));
 ++frame_submissions;
 memcpy(frame,map,sizeof(frame));
 lv_display_flush_ready(d);
}
esp_err_t display_service_init(void) {
 lv_init(); mock_display=lv_display_create(360,360);
 lv_display_set_color_format(mock_display,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(mock_display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_FULL);
 lv_display_set_flush_cb(mock_display,host_flush); return 0;
}
void display_service_deinit(void) { lv_deinit(); }
lv_display_t *display_service_get_display(void) { return mock_display; }
esp_err_t display_service_set_brightness(int n) { return 0; }
static void dump_frame(const char *path) {
 FILE*f=fopen(path,"wb"); assert(f); fprintf(f,"P6\n360 360\n255\n");
 for(int i=0;i<360*360;++i){uint16_t c=frame[i]; unsigned char rgb[3]={(c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31}; fwrite(rgb,1,3,f);} fclose(f);
}
static void finish_page_slide(void) {
 if(s_menu_sliding) {
  mock_time_ms+=MENU_SLIDE_MS;
  update_menu_slide(mock_time_ms);
 }
 render(); lv_refr_now(mock_display);
}
static void save(const char *path,ui_page_t page) {
 set_page(page); finish_page_slide(); dump_frame(path);
}
static uint32_t frame_hash(void) {
 uint32_t hash=2166136261u;
 for(unsigned i=0;i<360*360;++i) hash=(hash^frame[i])*16777619u;
 return hash;
}
static void menu_slide_checks(void) {
 s_state.status=UI_STATUS_WAIT_WAKE;
 set_page(UI_PAGE_HOME); finish_page_slide();
 uint16_t *home=malloc(sizeof(frame)); assert(home); memcpy(home,frame,sizeof(frame));
 unsigned events_before=event_count, cancels_before=touch_cancel_count;
 handle_touch((ui_touch_result_t){UI_GESTURE_UP,180,230});
 render(); lv_refr_now(mock_display);
 assert(s_page==UI_PAGE_SETTINGS && s_menu_sliding);
 assert(lv_obj_get_y(s_menu_layer)==360);
 assert(memcmp(home,frame,sizeof(frame))==0); /* 第一帧不先从顶部刷出菜单。 */
 if(!missing_assets) dump_frame("work/ui-checks/menu-open-00.ppm");
 int previous_y=360;
 for(unsigned step=1;step<=5;++step) {
  /* 滑动中点击/反向手势均不触发按钮，也不能穿透到背景首页。 */
  handle_touch((ui_touch_result_t){UI_GESTURE_TAP,80,100});
  handle_touch((ui_touch_result_t){UI_GESTURE_DOWN,180,300});
  assert(event_count==events_before && s_page==UI_PAGE_SETTINGS);
  mock_time_ms+=50; update_menu_slide(mock_time_ms);
  if(s_dirty) render();
  unsigned submissions_before=frame_submissions;
  lv_refr_now(mock_display);
  assert(frame_submissions==submissions_before+1); /* 菜单一帧只提交一次，不能分块刷新。 */
  int y=lv_obj_get_y(s_menu_layer);
  assert(y>=0 && y<previous_y); /* 菜单上沿单调向上，250ms 内落在 y=0。 */
  assert(memcmp(home,frame,(size_t)y*360*sizeof(uint16_t))==0);
  if(!missing_assets) {char path[100]; snprintf(path,sizeof(path),"work/ui-checks/menu-open-%02u.ppm",step); dump_frame(path);}
  previous_y=y;
 }
 assert(!s_menu_sliding && previous_y==0);
 assert(touch_cancel_count==cancels_before+2); /* 起止均切换手势代次。 */
 assert(lv_obj_has_flag(s_home_layer,LV_OBJ_FLAG_HIDDEN));
 handle_touch((ui_touch_result_t){UI_GESTURE_DOWN,180,300});
 render(); lv_refr_now(mock_display);
 assert(s_page==UI_PAGE_HOME && s_menu_sliding && lv_obj_get_y(s_menu_layer)==0);
 if(!missing_assets) dump_frame("work/ui-checks/menu-close-00.ppm");
 previous_y=0;
 for(unsigned step=1;step<=5;++step) {
  handle_touch((ui_touch_result_t){UI_GESTURE_TAP,180,160});
  assert(event_count==events_before);
  mock_time_ms+=50; update_menu_slide(mock_time_ms);
  if(s_dirty) render();
  unsigned submissions_before=frame_submissions;
  lv_refr_now(mock_display);
  assert(frame_submissions==submissions_before+1);
  int y=lv_obj_get_y(s_menu_layer);
  assert(y>previous_y && y<=360);
  if(!missing_assets) {char path[100]; snprintf(path,sizeof(path),"work/ui-checks/menu-close-%02u.ppm",step); dump_frame(path);}
  previous_y=y;
 }
 assert(!s_menu_sliding && lv_obj_has_flag(s_menu_layer,LV_OBJ_FLAG_HIDDEN));
 assert(!lv_obj_has_flag(s_home_layer,LV_OBJ_FLAG_HIDDEN));
 /* 后台配网可覆盖滑入/滑出，不能卡在半屏或继续沿用旧动画位置。 */
 handle_touch((ui_touch_result_t){UI_GESTURE_UP,180,230}); render();
 mock_time_ms+=100; update_menu_slide(mock_time_ms);
 set_page(UI_PAGE_PROVISION); render(); lv_refr_now(mock_display);
 assert(!s_menu_sliding && s_page==UI_PAGE_PROVISION && lv_obj_get_y(s_menu_layer)==0);
 mock_time_ms+=1000; update_menu_slide(mock_time_ms);
 assert(lv_obj_get_y(s_menu_layer)==0);
 handle_touch((ui_touch_result_t){UI_GESTURE_DOWN,180,300}); assert(s_page==UI_PAGE_PROVISION);
 set_page(UI_PAGE_HOME); finish_page_slide();
 handle_touch((ui_touch_result_t){UI_GESTURE_UP,180,230}); finish_page_slide();
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,250,275});
 assert(s_page==UI_PAGE_HOME && s_menu_sliding); /* 返回按钮同样向下收起。 */
 render(); mock_time_ms+=50; update_menu_slide(mock_time_ms);
 set_page(UI_PAGE_PROVISION); render(); lv_refr_now(mock_display);
 assert(!s_menu_sliding && lv_obj_get_y(s_menu_layer)==0);
 assert(event_count==events_before);
 set_page(UI_PAGE_HOME); finish_page_slide();
 free(home);
 puts("Menu slide checks passed: one complete LVGL frame per step, upward reveal, downward dismiss, transition touch isolation, provisioning override");
}
static void animation_checks(bool export_frames) {
 const ui_status_t states[]={UI_STATUS_WAIT_WAKE,UI_STATUS_LISTENING,UI_STATUS_SPEAKING,UI_STATUS_CANCELLING,UI_STATUS_WAIT_REPLY};
 const char *names[]={"idle","listening","speaking","cancelling","thinking"};
 unsigned events_before=event_count;
 uint32_t listening_frames[100];
 for(unsigned s=0;s<sizeof(states)/sizeof(*states);++s) {
  s_state.status=states[s]; set_page(UI_PAGE_HOME); finish_page_slide(); mock_time_ms+=5000;
  render(); lv_refr_now(mock_display);
  uint32_t first=frame_hash(); bool changed=false;
  for(unsigned f=0;f<100;++f) {
   /* 模拟取消确认到达：从 cancelling 转为 listening，画面应延续而非重播首帧。 */
   ui_status_t status=states[s]==UI_STATUS_CANCELLING && f>=40 ? UI_STATUS_LISTENING : s_state.status;
   ui_face_update(status,true,mock_time_ms); lv_refr_now(mock_display);
   changed |= first!=frame_hash();
   if(states[s]==UI_STATUS_LISTENING) listening_frames[f]=frame_hash();
   if(states[s]==UI_STATUS_CANCELLING) assert(frame_hash()==listening_frames[f]);
   if(export_frames) { char path[100]; snprintf(path,sizeof(path),"work/ui-checks/%s-%02u.ppm",names[s],f); dump_frame(path); }
   mock_time_ms+=50;
  }
  if(!missing_assets) assert(changed); /* 验证完整周期的生产画面，不只检查计时器。 */
 }
 assert(event_count==events_before); /* 动画不能触发任何对话/配置动作。 */
 assert(asset_open_count==1); /* 多状态切换不重复映射/申请资源。 */
 set_page(UI_PAGE_SETTINGS); finish_page_slide();
 uint32_t menu=frame_hash();
 ui_face_update(UI_STATUS_SPEAKING,false,mock_time_ms+1000); lv_refr_now(mock_display);
 assert(menu==frame_hash()); /* 菜单不会被后台表情覆盖。 */
 puts("Production animation checks passed: cancelling displays listening immediately and continues after acknowledgement; menu stays unchanged, no business events");
}
int main(int argc,char **argv) {
 missing_assets=argc>1 && strcmp(argv[1],"--missing-assets")==0;
 assert(ui_service_init()==0);
 s_state.status=UI_STATUS_WAIT_WAKE; s_state.volume=40; s_state.brightness=70; s_state.wifi_connected=true;
 snprintf(s_state.ssid,sizeof(s_state.ssid),"Phooooone");
 snprintf(s_state.ap_ssid,sizeof(s_state.ap_ssid),"YMOS-32E1");
 snprintf(s_state.provisioning_url,sizeof(s_state.provisioning_url),"http://192.168.4.1");
 save("work/ui-checks/home.ppm",UI_PAGE_HOME);
 save("work/ui-checks/settings.ppm",UI_PAGE_SETTINGS);
 unsigned before=event_count;
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,80,100});
 assert(event_count==before+1 && last_event==EV_BUTTON_VOLUME_DOWN);
 before=event_count; handle_touch((ui_touch_result_t){UI_GESTURE_TAP,180,225}); assert(event_count==before);
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,280,160}); assert(last_event==EV_UI_BRIGHTNESS_UP);
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,100,270}); assert(s_page==UI_PAGE_CONFIRM);
 save("work/ui-checks/confirm.ppm",UI_PAGE_CONFIRM);
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,240,255}); assert(last_event==EV_UI_PROVISION);
 before=event_count; handle_touch((ui_touch_result_t){UI_GESTURE_TAP,240,255}); assert(event_count==before);
 save("work/ui-checks/provision.ppm",UI_PAGE_PROVISION);
 handle_touch((ui_touch_result_t){UI_GESTURE_TAP,180,300}); assert(last_event==EV_UI_PROVISION_EXIT);
 set_page(UI_PAGE_HOME); finish_page_slide(); handle_touch((ui_touch_result_t){UI_GESTURE_TAP,180,160}); assert(last_event==EV_UI_TALK);
 menu_slide_checks();
 animation_checks(argc>1 && strcmp(argv[1],"--animation-frames")==0);
 ui_face_deinit(); display_service_deinit();
 puts("Production UI render and action routing checks passed (mock RTOS/display transport)");
 return 0;
}
