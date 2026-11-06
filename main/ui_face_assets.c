/* 显示资源与应用/模型/NVS 分区分离。缺失或版本不符时由 UI 使用静态降级画面。 */
#include "ui_face_assets.h"
#include "esp_partition.h"
#include <string.h>

static const void *s_mapping;
static esp_partition_mmap_handle_t s_handle;
static ui_animation_t s_animations[UI_ANIM_COUNT];

void ui_face_assets_close(void)
{
    if (s_mapping) esp_partition_munmap(s_handle);
    s_mapping = NULL;
    memset(s_animations, 0, sizeof(s_animations));
}

esp_err_t ui_face_assets_open(ui_animation_t animations[UI_ANIM_COUNT])
{
    if (!animations) return ESP_ERR_INVALID_ARG;
    if (!s_mapping) {
        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ui_assets");
        if (!partition) return ESP_ERR_NOT_FOUND;
        esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
            ESP_PARTITION_MMAP_DATA, &s_mapping, &s_handle);
        if (err != ESP_OK) { s_mapping = NULL; return err; }
        if (!ui_animation_pack_open(s_mapping, partition->size, s_animations)) {
            ui_face_assets_close();
            return ESP_ERR_INVALID_CRC;
        }
    }
    memcpy(animations, s_animations, sizeof(s_animations));
    return ESP_OK;
}
