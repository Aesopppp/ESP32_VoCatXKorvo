/* 显示服务测试的外设/LVGL API 替身，仅供主机编译，不参与固件。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_8BIT 8
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *pointer);
void *spi_bus_dma_memory_alloc(int host, size_t size, uint32_t caps);

enum { SPI2_HOST = 2, SPI_DMA_CH_AUTO = 3 };
enum { GPIO_NUM_11=11, GPIO_NUM_12=12, GPIO_NUM_13=13, GPIO_NUM_14=14,
       GPIO_NUM_18=18, GPIO_NUM_44=44, GPIO_NUM_46=46, GPIO_NUM_47=47 };
typedef struct { size_t max_transfer_sz; } spi_bus_config_t;
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma);
esp_err_t spi_bus_free(int host);

typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef bool (*lcd_done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
typedef struct {
    size_t trans_queue_depth;
    unsigned pclk_hz;
    lcd_done_cb_t on_color_trans_done;
    void *user_ctx;
    struct { bool psram_dma_direct; } flags;
} esp_lcd_panel_io_spi_config_t;
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; } st77916_lcd_init_cmd_t;
typedef struct {
    const st77916_lcd_init_cmd_t *init_cmds;
    size_t init_cmds_size;
    struct { bool use_qspi_interface; } flags;
} st77916_vendor_config_t;
typedef struct {
    int reset_gpio_num, rgb_ele_order, bits_per_pixel;
    struct { bool reset_active_high; } flags;
    const void *vendor_config;
} esp_lcd_panel_dev_config_t;
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define ST77916_PANEL_BUS_QSPI_CONFIG(clk,d0,d1,d2,d3,bytes) { .max_transfer_sz=(bytes) }
#define ST77916_PANEL_IO_QSPI_CONFIG(cs,cb,ctx) \
    { .pclk_hz=40000000, .on_color_trans_done=(cb), .user_ctx=(ctx) }
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t, const esp_lcd_panel_io_spi_config_t *, esp_lcd_panel_io_handle_t *);
esp_err_t esp_lcd_new_panel_st77916(esp_lcd_panel_io_handle_t, const esp_lcd_panel_dev_config_t *, esp_lcd_panel_handle_t *);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);

enum { LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_TIMER_10_BIT, LEDC_TIMER_0, LEDC_AUTO_CLK };
typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, timer_sel; unsigned duty; } ledc_channel_config_t;
esp_err_t ledc_stop(int, int, int);
esp_err_t ledc_timer_config(const ledc_timer_config_t *);
esp_err_t ledc_channel_config(const ledc_channel_config_t *);
esp_err_t ledc_set_duty(int, int, uint32_t);
esp_err_t ledc_update_duty(int, int);

/* 真实 LVGL 的绘图和动画另由 ui_host/render.c 验证，这里仅保存注册的刷新接口。 */
#define LV_USE_STDLIB_MALLOC 1
#define LV_STDLIB_CUSTOM 0
enum { LV_COLOR_FORMAT_RGB565, LV_DISPLAY_RENDER_MODE_FULL };
typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef struct lv_display_t lv_display_t;
typedef void (*test_flush_cb_t)(lv_display_t *, const lv_area_t *, uint8_t *);
struct lv_display_t {
    test_flush_cb_t flush_cb;
    void *pixels;
    size_t bytes;
    unsigned completed;
};
void lv_init(void);
void lv_deinit(void);
void lv_tick_set_cb(uint32_t (*tick)(void));
lv_display_t *lv_display_create(int width, int height);
void lv_display_delete(lv_display_t *);
void lv_display_set_color_format(lv_display_t *, int);
void lv_display_set_buffers(lv_display_t *, void *, void *, size_t, int);
void lv_display_set_flush_cb(lv_display_t *, test_flush_cb_t);
void lv_display_flush_ready(lv_display_t *);
void lv_draw_sw_rgb565_swap(void *pixels, size_t count);
