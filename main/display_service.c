/* ST77916 QSPI + LVGL 整帧合成；通过固定内部 RAM 缓冲分段 DMA 发送。 */
#include "display_service.h"
#include "board_config.h"
#include "display_panel_init.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui_policy.h"
#include <string.h>

static const char *TAG = "display_service";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static void *s_buffer, *s_dma_buffer;
static SemaphoreHandle_t s_flush_done;
static bool s_spi_ready, s_pwm_ready, s_lvgl_ready;
static bool s_flush_failed;
#define FRAME_BYTES (BSP_LCD_WIDTH * BSP_LCD_HEIGHT * sizeof(uint16_t))
#define DMA_LINES 20
#define DMA_BYTES (BSP_LCD_WIDTH * DMA_LINES * sizeof(uint16_t))
#define DMA_WAIT_MS 100 /* 20 行纯传输约 0.72 ms；超时按显示故障处理，不重用在途缓冲。 */

/* LVGL 自定义分配器。图形对象、字体缓存等不抢占音频/TLS 的内部堆。
 * 不回退到内部 RAM；分配失败由 LVGL 的错误/断言机制暴露。 */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *lv_realloc_core(void *p, size_t size) { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void lv_free_core(void *p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *m)
{
    memset(m, 0, sizeof(*m));
    m->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    m->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif

static uint32_t lvgl_tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io; (void)event; (void)context;
    /* ISR 只释放 DMA 完成信号；LVGL 全部由 UI 任务操作。 */
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    /* FULL 模式在这里收到已经画完的一帧。传输阶段只做拷贝，不再穿插绘图。
     * 实板已出现 PSRAM 直传 DMA 欠载；DMA 只读内部 RAM，避免外部总线争用。
     * 单个中转缓冲必须等完成再覆盖；源帧不做字节序转换，保持 LVGL 原生格式。 */
    size_t width = (size_t)(area->x2 - area->x1 + 1);
    for (int y = area->y1; !s_flush_failed && y <= area->y2; y += DMA_LINES) {
        int rows = area->y2 - y + 1;
        if (rows > DMA_LINES) rows = DMA_LINES;
        size_t count = width * (size_t)rows;
        memcpy(s_dma_buffer, pixels + (size_t)(y - area->y1) * width * sizeof(uint16_t),
               count * sizeof(uint16_t));
        lv_draw_sw_rgb565_swap(s_dma_buffer, count);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, area->x1, y,
                                                 area->x2 + 1, y + rows, s_dma_buffer);
        if (err == ESP_OK && xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(DMA_WAIT_MS)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        }
        if (err != ESP_OK) {
            /* IO 回收失败后不能继续提交，否则驱动可能永远等待已出队的事务。
             * 超时也不能假定 DMA 已停止：保留中转缓冲且停止复用，直到设备重启。
             * LVGL 源帧未交给 DMA，仍可释放本次刷新，使 UI 任务继续处理消息。 */
            s_flush_failed = true;
            ESP_LOGE(TAG, "LCD transfer failed at row %d: %s; refresh halted until reboot",
                     y, esp_err_to_name(err));
        }
    }
    /* 传输等待已在本回调内完成并让出 CPU，无须额外的 LVGL flush_wait 回调。 */
    lv_display_flush_ready(display);
}

void display_service_deinit(void)
{
    if (s_pwm_ready) { ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0); s_pwm_ready = false; }
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io) { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_flush_done) { vSemaphoreDelete(s_flush_done); s_flush_done = NULL; }
    if (s_display) { lv_display_delete(s_display); s_display = NULL; }
    heap_caps_free(s_buffer); s_buffer = NULL;
    heap_caps_free(s_dma_buffer); s_dma_buffer = NULL;
    if (s_spi_ready) { spi_bus_free(SPI2_HOST); s_spi_ready = false; }
    if (s_lvgl_ready) { lv_deinit(); s_lvgl_ready = false; }
    s_flush_failed = false;
}

esp_err_t display_service_init(void)
{
    if (s_display) return ESP_OK;
    esp_err_t err;
    spi_bus_config_t bus = ST77916_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_SCLK_PIN, BSP_LCD_DATA0_PIN, BSP_LCD_DATA1_PIN,
        BSP_LCD_DATA2_PIN, BSP_LCD_DATA3_PIN, DMA_BYTES);
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    s_spi_ready = true;
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_lcd_panel_io_spi_config_t io = ST77916_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS_PIN, flush_done, NULL);
    io.trans_queue_depth = 1; /* 每次仅一个内部 RAM 条带在途，完成后才复用。 */
    io.flags.psram_dma_direct = false; /* 此板上已复现直传欠载，禁止 PSRAM 直接 DMA。 */
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io);
    if (err != ESP_OK) goto fail;
    st77916_vendor_config_t vendor = {
        .init_cmds = vocat_panel_init_commands,
        .init_cmds_size = sizeof(vocat_panel_init_commands) / sizeof(vocat_panel_init_commands[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = BSP_LCD_RESET_PIN, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .flags.reset_active_high = true, .vendor_config = &vendor,
    };
    err = esp_lcd_new_panel_st77916(s_io, &panel, &s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(s_panel, false);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(s_panel, false, false);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) goto fail;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer);
    if (err != ESP_OK) goto fail;
    ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT_PIN, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) goto fail;
    s_pwm_ready = true;
    /* 绘图与 DMA 缓冲各在启动时申请一次；整帧不回退到内部 RAM。 */
    s_buffer = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dma_buffer = spi_bus_dma_memory_alloc(SPI2_HOST, DMA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buffer || !s_dma_buffer) { err = ESP_ERR_NO_MEM; goto fail; }
    lv_init(); s_lvgl_ready = true;
    lv_tick_set_cb(lvgl_tick_ms);
    s_display = lv_display_create(BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    if (!s_display) { err = ESP_ERR_NO_MEM; goto fail; }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_buffer, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_display, flush);
    ESP_LOGI(TAG, "ST77916 ready: full-frame=%u bytes in PSRAM, internal DMA=%u bytes, direct PSRAM DMA off",
             (unsigned)FRAME_BYTES, (unsigned)DMA_BYTES);
    return ESP_OK;
fail:
    display_service_deinit();
    return err;
}

lv_display_t *display_service_get_display(void) { return s_display; }

esp_err_t display_service_set_brightness(int percent)
{
    if (!s_pwm_ready) return ESP_ERR_INVALID_STATE;
    if (percent < UI_BRIGHTNESS_MIN || percent > UI_BRIGHTNESS_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)percent * 1023 / 100);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) : err;
}
