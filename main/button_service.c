/** VoCat BOOT 长按配网；扫描任务只发布事件，业务由 app_main 消费。 */
#include "button_service.h"
#include "boot_button_policy.h"
#include "board_config.h"
#include "event_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "button_service";
static TaskHandle_t s_task;

static void button_task(void *arg)
{
    (void)arg;
    button_policy_t policy = button_policy_initial();
    while (1) {
        bool pressed = gpio_get_level(BSP_BOOT_BUTTON_PIN) == 0;
        button_action_t action = boot_button_policy_feed(
            &policy, pressed, (uint32_t)(esp_timer_get_time() / 1000));
        if (action == BUTTON_ACTION_PROVISION) {
            if (event_bus_publish(EV_BUTTON_PROVISION, NULL, 0) != pdPASS)
                ESP_LOGW(TAG, "BOOT event dropped: bus full; release and hold again");
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_service_init(void)
{
    if (s_task) return ESP_OK;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BSP_BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    if (xTaskCreate(button_task, "boot_button", 3072, NULL, 3, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "BOOT ready: GPIO0, hold 3s for Wi-Fi setup; short press has no action");
    return ESP_OK;
}
