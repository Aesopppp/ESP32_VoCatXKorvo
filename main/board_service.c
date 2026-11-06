/** ESP-VoCat v1.2 板级初始化；不依赖显示、触摸或 Korvo IO 扩展器。 */
#include "board_service.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_service";
static i2c_master_bus_handle_t s_audio_bus;

i2c_master_bus_handle_t board_service_get_i2c_bus(void) { return s_audio_bus; }

esp_err_t board_service_init(void)
{
    if (s_audio_bus) return ESP_OK;

    /* 先设输出锁存值再使能输出，软复位时不主动拉低音频电源。
     * GPIO9 保持原 VoCat 的低电平；本版不实现电源键关机。
     */
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_CODEC_POWER_PIN, 1), TAG, "codec power level");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_CTRL_PIN, 0), TAG, "power control level");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_BACKLIGHT_PIN, 0), TAG, "backlight level");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_PA_EN_PIN, 0), TAG, "PA initial level");
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BSP_CODEC_POWER_PIN) | (1ULL << BSP_POWER_CTRL_PIN) |
                        (1ULL << BSP_LCD_BACKLIGHT_PIN) | (1ULL << BSP_PA_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "board power pins");
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_config_t config = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_PIN,
        .scl_io_num = BSP_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_audio_bus), TAG, "audio I2C bus");

    /* 有界等待上电稳定，只探测本板的 Codec。 */
    esp_err_t dac = ESP_FAIL, adc = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        dac = i2c_master_probe(s_audio_bus, ES8311_I2C_ADDR, 50);
        adc = i2c_master_probe(s_audio_bus, ES7210_I2C_ADDR, 50);
        if (dac == ESP_OK && adc == ESP_OK) {
            ESP_LOGI(TAG, "VoCat v1.2 ready: codec power=GPIO48, SDA=2 SCL=1, ES8311=0x18 ES7210=0x40");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "Codec probe failed: ES8311=%s ES7210=%s; check VoCat v1.2 power",
             esp_err_to_name(dac), esp_err_to_name(adc));
    i2c_del_master_bus(s_audio_bus);
    s_audio_bus = NULL;
    return dac != ESP_OK ? dac : adc;
}
