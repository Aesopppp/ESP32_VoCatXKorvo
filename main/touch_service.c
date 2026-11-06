/* CST816S：独立任务采样，UI 只接收完整手势；刷新屏幕不会阻塞触摸读点。 */
#include "touch_service.h"
#include "board_service.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "touch_service";
static i2c_master_dev_handle_t s_touch;
static QueueHandle_t s_gestures;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static atomic_bool s_exit;
static atomic_uint s_page_generation;
static bool s_isr_ready;

typedef struct {
    ui_touch_result_t result;
    unsigned generation;
} touch_message_t;

static void touch_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void touch_task(void *arg)
{
    (void)arg;
    ui_touch_policy_t policy = {0};
    unsigned generation = atomic_load(&s_page_generation);
    bool ignore_until_release = false, read_failed = false;
    while (!atomic_load(&s_exit)) {
        /* 按住期间补充读点；空闲或 I2C 出错时等待下一次 IRQ，不持续占用总线。 */
        TickType_t wait = !read_failed && (policy.pressed || ignore_until_release)
                         ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
        if (atomic_load(&s_exit)) break;
        unsigned current_generation = atomic_load(&s_page_generation);
        if (current_generation != generation) {
            generation = current_generation;
            /* 只丢弃已按下的手势；空闲触控可能 NACK，不能因此吃掉新页面第一次点击。 */
            ignore_until_release = ignore_until_release || policy.pressed;
            ui_touch_cancel(&policy);
        }
        uint8_t reg = 0x02, data[6];
        esp_err_t err = i2c_master_transmit_receive(s_touch, &reg, 1, data, sizeof(data), 30);
        if (err != ESP_OK) {
            ignore_until_release = ignore_until_release || policy.pressed;
            ui_touch_cancel(&policy);
            read_failed = true;
            continue;
        }
        read_failed = false;
        bool down = (data[0] & 0x0f) != 0;
        if (ignore_until_release) {
            if (!down) ignore_until_release = false;
            continue;
        }
        int x = ((data[1] & 0x0f) << 8) | data[2];
        int y = ((data[3] & 0x0f) << 8) | data[4];
        if (down && (x >= BSP_LCD_WIDTH || y >= BSP_LCD_HEIGHT || (data[0] & 0x0f) != 1)) {
            ui_touch_cancel(&policy);
            ignore_until_release = true;
            continue;
        }
        touch_message_t message = {.result = ui_touch_feed(&policy, down, x, y), .generation = generation};
        /* 满时丢弃新手势，不阻塞采样，也不把未完整采样的手势解释为点击。 */
        if (message.result.gesture != UI_GESTURE_NONE) xQueueSend(s_gestures, &message, 0);
    }
    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

void touch_service_cancel(void)
{
    /* UI 不访问采样任务的手势状态；代次同时作废队列中旧页面的动作。 */
    atomic_fetch_add(&s_page_generation, 1);
    if (s_task) xTaskNotifyGive(s_task);
}

void touch_service_deinit(void)
{
    if (s_isr_ready) {
        gpio_intr_disable(BSP_TOUCH_INT_PIN);
        gpio_isr_handler_remove(BSP_TOUCH_INT_PIN);
        s_isr_ready = false;
    }
    if (s_task) {
        /* 等当前有界 I2C 事务退出，再释放设备句柄；不能强杀正在读点的任务。 */
        atomic_store(&s_exit, true);
        xTaskNotifyGive(s_task);
        xSemaphoreTake(s_stopped, portMAX_DELAY);
        s_task = NULL;
    }
    if (s_touch) { i2c_master_bus_rm_device(s_touch); s_touch = NULL; }
    if (s_gestures) { vQueueDelete(s_gestures); s_gestures = NULL; }
    if (s_stopped) { vSemaphoreDelete(s_stopped); s_stopped = NULL; }
}

esp_err_t touch_service_init(void)
{
    if (s_task) return ESP_OK;
    i2c_master_bus_handle_t bus = board_service_get_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &device, &s_touch);
    if (err != ESP_OK) return err;
    s_gestures = xQueueCreate(8, sizeof(touch_message_t));
    s_stopped = xSemaphoreCreateBinary();
    if (!s_gestures || !s_stopped) { err = ESP_ERR_NO_MEM; goto fail; }
    gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << BSP_TOUCH_INT_PIN, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_ANYEDGE,
    };
    err = gpio_config(&interrupt);
    if (err != ESP_OK) goto fail;
    err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK; /* 其他驱动已安装全局服务。 */
    if (err != ESP_OK) goto fail;
    atomic_store(&s_exit, false);
    atomic_store(&s_page_generation, 0);
    if (xTaskCreatePinnedToCore(touch_task, "vocat_touch", 3072, NULL, 3, &s_task, 0) != pdPASS) {
        err = ESP_ERR_NO_MEM; goto fail;
    }
    err = gpio_isr_handler_add(BSP_TOUCH_INT_PIN, touch_isr, NULL);
    if (err != ESP_OK) goto fail;
    s_isr_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "CST816S ready: shared I2C0 addr=0x15 INT=GPIO10");
    return ESP_OK;
fail:
    touch_service_deinit();
    return err;
}

ui_touch_result_t touch_service_poll(void)
{
    touch_message_t message;
    while (s_gestures && xQueueReceive(s_gestures, &message, 0) == pdTRUE)
        if (message.generation == atomic_load(&s_page_generation)) return message.result;
    return (ui_touch_result_t){0};
}
