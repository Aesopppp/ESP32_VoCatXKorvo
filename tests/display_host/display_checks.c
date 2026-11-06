/* 编译执行真实 display_service.c，替换 SDK 外设调用，验证内存归属和传输生命周期。
 * 不模拟真实 PSRAM 带宽或 LCD 扫描；硬件欠载的消失仍须实板验证。 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/display_service.c"

static struct { void *pointer; size_t bytes; uint32_t caps; } allocations[8];
static unsigned allocation_calls, fail_allocation, draw_calls, fail_draw, dma_waits;
static bool drop_completion, pending;
static int semaphore_value, tx_x1, tx_y1, tx_x2, tx_y2;
static size_t bus_capacity, tx_bytes;
static const void *tx_pixels;
static uint8_t wire_copy[DMA_BYTES];
static uint16_t panel_pixels[BSP_LCD_WIDTH * BSP_LCD_HEIGHT], source_copy[BSP_LCD_WIDTH * BSP_LCD_HEIGHT];
static esp_lcd_panel_io_spi_config_t io_config;
static lv_display_t display;

static unsigned allocation_index(const void *pointer)
{
    for (unsigned i = 0; i < 8; ++i) if (allocations[i].pointer == pointer) return i;
    assert(!"untracked buffer"); return 0;
}

void *heap_caps_malloc(size_t bytes, uint32_t caps)
{
    if (++allocation_calls == fail_allocation) return NULL;
    for (unsigned i = 0; i < 8; ++i) if (!allocations[i].pointer) {
        allocations[i].pointer = calloc(1, bytes); assert(allocations[i].pointer);
        allocations[i].bytes = bytes; allocations[i].caps = caps;
        return allocations[i].pointer;
    }
    assert(!"allocation limit exceeded"); return NULL;
}
void heap_caps_free(void *pointer)
{
    if (!pointer) return;
    assert(!pending || pointer != tx_pixels);
    unsigned i = allocation_index(pointer);
    free(pointer); memset(&allocations[i], 0, sizeof(allocations[i]));
}
void *spi_bus_dma_memory_alloc(int host, size_t bytes, uint32_t caps)
{
    assert(host == SPI2_HOST && (caps & MALLOC_CAP_INTERNAL));
    assert(!(caps & MALLOC_CAP_SPIRAM) && bytes <= bus_capacity);
    return heap_caps_malloc(bytes, caps | MALLOC_CAP_DMA);
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma)
{
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    bus_capacity = bus->max_transfer_sz; return ESP_OK;
}
esp_err_t spi_bus_free(int host) { return ESP_OK; }
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t *config,
                                  esp_lcd_panel_io_handle_t *output)
{
    assert(!config->flags.psram_dma_direct && config->trans_queue_depth == 1);
    io_config = *config; *output = (void *)1; return ESP_OK;
}
esp_err_t esp_lcd_new_panel_st77916(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *config,
                                   esp_lcd_panel_handle_t *output)
{ *output = (void *)2; return ESP_OK; }
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { return ESP_OK; }
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p, bool v) { return ESP_OK; }
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t p, bool x, bool y) { return ESP_OK; }
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { return ESP_OK; }
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p) { return ESP_OK; }
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t p) { assert(!pending); return ESP_OK; }

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *pixels)
{
    assert(!pending); /* 下一个条带不能抢占仍由 DMA 使用的缓冲。 */
    ++draw_calls;
    if (draw_calls == fail_draw) return ESP_ERR_INVALID_STATE;
    unsigned index = allocation_index(pixels);
    assert((allocations[index].caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) ==
           (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    assert(!(allocations[index].caps & MALLOC_CAP_SPIRAM));
    assert(x1 >= 0 && y1 >= 0 && x2 <= BSP_LCD_WIDTH && y2 <= BSP_LCD_HEIGHT);
    assert(x2 > x1 && y2 > y1);
    tx_bytes = (size_t)(x2 - x1) * (y2 - y1) * 2;
    assert(tx_bytes <= allocations[index].bytes && tx_bytes <= bus_capacity);
    tx_x1 = x1; tx_x2 = x2; tx_y1 = y1; tx_y2 = y2; tx_pixels = pixels;
    memcpy(wire_copy, pixels, tx_bytes);
    pending = true; return ESP_OK;
}
static void complete_transfer(void)
{
    assert(pending && memcmp(tx_pixels, wire_copy, tx_bytes) == 0);
    size_t offset = 0;
    for (int y = tx_y1; y < tx_y2; ++y) for (int x = tx_x1; x < tx_x2; ++x) {
        panel_pixels[y * BSP_LCD_WIDTH + x] = (uint16_t)((wire_copy[offset] << 8) | wire_copy[offset + 1]);
        offset += 2;
    }
    pending = false;
    io_config.on_color_trans_done((void *)1, NULL, io_config.user_ctx);
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { semaphore_value = 0; return &semaphore_value; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{ assert(sem == &semaphore_value && semaphore_value == 0); semaphore_value = 1; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem == &semaphore_value && ticks > 0 && ticks <= pdMS_TO_TICKS(100));
    ++dma_waits;
    if (drop_completion) return pdFALSE;
    complete_transfer();
    assert(semaphore_value == 1); semaphore_value = 0; return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(!pending); }

esp_err_t ledc_stop(int m, int c, int d) { return ESP_OK; }
esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { return ESP_OK; }
esp_err_t ledc_set_duty(int m, int c, uint32_t d) { return ESP_OK; }
esp_err_t ledc_update_duty(int m, int c) { return ESP_OK; }
int64_t esp_timer_get_time(void) { return 0; }
void lv_init(void) {}
void lv_deinit(void) {}
void lv_tick_set_cb(uint32_t (*tick)(void)) {}
lv_display_t *lv_display_create(int width, int height)
{ assert(width == BSP_LCD_WIDTH && height == BSP_LCD_HEIGHT); memset(&display, 0, sizeof(display)); return &display; }
void lv_display_delete(lv_display_t *d) {}
void lv_display_set_color_format(lv_display_t *d, int format) { assert(format == LV_COLOR_FORMAT_RGB565); }
void lv_display_set_buffers(lv_display_t *d, void *a, void *b, size_t bytes, int mode)
{
    assert(!b && bytes == FRAME_BYTES && mode == LV_DISPLAY_RENDER_MODE_FULL);
    assert(allocations[allocation_index(a)].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    d->pixels = a; d->bytes = bytes;
}
void lv_display_set_flush_cb(lv_display_t *d, test_flush_cb_t cb) { d->flush_cb = cb; }
void lv_display_flush_ready(lv_display_t *d) { assert(!pending || drop_completion); ++d->completed; }
void lv_draw_sw_rgb565_swap(void *pixels, size_t count)
{
    uint8_t *bytes = pixels;
    for (size_t i = 0; i < count * 2; i += 2) { uint8_t t = bytes[i]; bytes[i] = bytes[i + 1]; bytes[i + 1] = t; }
}

static void reset_test(void)
{
    /* 测试替身主动结束迟到 DMA 后才能清理；固件故障分支实际保留缓冲直到重启。 */
    if (pending) complete_transfer();
    display_service_deinit();
    for (unsigned i = 0; i < 8; ++i) assert(!allocations[i].pointer);
    allocation_calls = fail_allocation = draw_calls = fail_draw = dma_waits = 0;
    drop_completion = false;
}

int main(void)
{
    for (unsigned failure = 1; failure <= 2; ++failure) {
        fail_allocation = failure;
        assert(display_service_init() == ESP_ERR_NO_MEM && display_service_get_display() == NULL);
        reset_test();
    }
    assert(display_service_init() == ESP_OK);
    unsigned initial_allocations = allocation_calls;
    lv_area_t full = {0, 0, BSP_LCD_WIDTH - 1, BSP_LCD_HEIGHT - 1};
    for (unsigned frame = 0; frame < 5; ++frame) {
        for (unsigned i = 0; i < FRAME_BYTES / 2; ++i) source_copy[i] = (uint16_t)(i * 31 + frame * 127);
        memcpy(display.pixels, source_copy, FRAME_BYTES);
        display.flush_cb(&display, &full, display.pixels);
        assert(memcmp(display.pixels, source_copy, FRAME_BYTES) == 0); /* 不改源帧字节序。 */
        assert(memcmp(panel_pixels, source_copy, FRAME_BYTES) == 0);
        assert(!s_flush_failed && !pending && display.completed == frame + 1);
    }
    assert(draw_calls == 90 && dma_waits == 90 && allocation_calls == initial_allocations);
    /* 尾块不足 20 行时，仍需保持行序/颜色且不越界。 */
    lv_area_t tail = {0, 3, BSP_LCD_WIDTH - 1, 357};
    memset(panel_pixels, 0, sizeof(panel_pixels));
    display.flush_cb(&display, &tail, display.pixels);
    assert(memcmp(panel_pixels + 3 * BSP_LCD_WIDTH, source_copy, 355 * BSP_LCD_WIDTH * 2) == 0);
    assert(panel_pixels[0] == 0 && panel_pixels[358 * BSP_LCD_WIDTH] == 0);
    reset_test();

    assert(display_service_init() == ESP_OK);
    fail_draw = 2;
    display.flush_cb(&display, &full, display.pixels);
    assert(s_flush_failed && draw_calls == 2 && dma_waits == 1 && display.completed == 1);
    display.flush_cb(&display, &full, display.pixels);
    assert(draw_calls == 2 && display.completed == 2); /* 出错后不再次进入损坏的 IO 队列。 */
    reset_test();

    assert(display_service_init() == ESP_OK);
    drop_completion = true;
    display.flush_cb(&display, &full, display.pixels);
    assert(s_flush_failed && pending && dma_waits == 1 && display.completed == 1);
    memset(display.pixels, 0x5a, FRAME_BYTES);
    display.flush_cb(&display, &full, display.pixels);
    assert(draw_calls == 1 && memcmp(tx_pixels, wire_copy, tx_bytes) == 0);
    complete_transfer(); /* 迟到完成不能解除故障或触发重用。 */
    display.flush_cb(&display, &full, display.pixels);
    assert(draw_calls == 1 && s_flush_failed);
    reset_test();
    puts("Display transport checks passed: internal-only DMA, full-frame pixels, tail rows, no per-frame allocation, init cleanup, send failure, timeout and late completion isolation");
    return 0;
}
