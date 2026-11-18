#include "device_mqtt_storage.h"
#include <string.h>

/* v2：4 字节魔数 + 1 字节 next/count + NUL 结尾的设备 ID。
 * history 按原环形槽位存 32 个 NUL 字符串，空槽只占 1 字节。
 * pending 只存 count 条：ID + 8 字节小端 Unix 秒 + state + failure_pending。
 * 无结构体 padding，不写空槽预留空间；最长 ID 和历史容量保持不变。
 * NVS 自身提供 CRC 与单键替换，不另外维护容易失步的长度/校验副本。 */
static bool put_string(uint8_t **cursor, const uint8_t *end, const char *value, size_t capacity)
{
    const char *nul = memchr(value, 0, capacity);
    if (!nul) return false;
    size_t bytes = (size_t)(nul - value) + 1;
    if ((size_t)(end - *cursor) < bytes) return false;
    memcpy(*cursor, value, bytes);
    *cursor += bytes;
    return true;
}

static bool get_string(const uint8_t **cursor, const uint8_t *end, char *value, size_t capacity)
{
    const uint8_t *nul = memchr(*cursor, 0, (size_t)(end - *cursor));
    if (!nul || (size_t)(nul - *cursor) >= capacity) return false;
    size_t bytes = (size_t)(nul - *cursor) + 1;
    memcpy(value, *cursor, bytes);
    *cursor += bytes;
    return true;
}

size_t device_mqtt_storage_encode_history(const device_mqtt_history_t *h, void *data, size_t capacity)
{
    if (!h || !data || capacity < 5 || h->version != 1 || h->next >= DEVICE_TASK_HISTORY_SIZE) return 0;
    uint8_t *start = data, *p = start + 5;
    const uint8_t *end = start + capacity;
    memcpy(start, "MQH2", 4);
    start[4] = (uint8_t)h->next;
    if (!put_string(&p, end, h->device_id, sizeof(h->device_id))) return 0;
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i)
        if (!put_string(&p, end, h->ids[i], sizeof(h->ids[i]))) return 0;
    return (size_t)(p - start);
}

size_t device_mqtt_storage_encode_pending(const mqtt_pending_store_t *pending, void *data, size_t capacity)
{
    if (!pending || !data || capacity < 5 || pending->version != MQTT_PENDING_VERSION ||
        pending->count > DEVICE_TASK_QUEUE_SIZE) return 0;
    uint8_t *start = data, *p = start + 5;
    const uint8_t *end = start + capacity;
    memcpy(start, "MQP2", 4);
    start[4] = (uint8_t)pending->count;
    if (!put_string(&p, end, pending->device_id, sizeof(pending->device_id))) return 0;
    for (unsigned i = 0; i < pending->count; ++i) {
        const mqtt_pending_entry_t *entry = &pending->entries[i];
        if (entry->received_at < 0 || !put_string(&p, end, entry->id, sizeof(entry->id)) ||
            (size_t)(end - p) < 10) return 0;
        uint64_t seconds = (uint64_t)entry->received_at;
        for (unsigned j = 0; j < 8; ++j) *p++ = (uint8_t)(seconds >> (j * 8));
        *p++ = entry->state;
        *p++ = entry->failure_pending;
    }
    return (size_t)(p - start);
}

bool device_mqtt_storage_decode_history(const void *data, size_t size, device_mqtt_history_t *h, bool *legacy)
{
    if (!data || !h || !legacy || size < 5) return false;
    *legacy = false;
    if (memcmp(data, "MQH2", 4)) {
        if (size != sizeof(*h)) return false;
        memcpy(h, data, size);
        *legacy = h->version == 1;
        return *legacy;
    }
    const uint8_t *start = data, *p = start + 5, *end = start + size;
    memset(h, 0, sizeof(*h));
    h->version = 1;
    h->next = start[4];
    if (h->next >= DEVICE_TASK_HISTORY_SIZE ||
        !get_string(&p, end, h->device_id, sizeof(h->device_id))) return false;
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i)
        if (!get_string(&p, end, h->ids[i], sizeof(h->ids[i]))) return false;
    return p == end;
}

bool device_mqtt_storage_decode_pending(const void *data, size_t size, mqtt_pending_store_t *pending, bool *legacy)
{
    if (!data || !pending || !legacy || size < 5) return false;
    *legacy = false;
    if (memcmp(data, "MQP2", 4)) {
        if (size != sizeof(*pending)) return false;
        memcpy(pending, data, size);
        *legacy = pending->version == MQTT_PENDING_VERSION;
        return *legacy;
    }
    const uint8_t *start = data, *p = start + 5, *end = start + size;
    memset(pending, 0, sizeof(*pending));
    pending->version = MQTT_PENDING_VERSION;
    pending->count = start[4];
    if (pending->count > DEVICE_TASK_QUEUE_SIZE ||
        !get_string(&p, end, pending->device_id, sizeof(pending->device_id))) return false;
    for (unsigned i = 0; i < pending->count; ++i) {
        mqtt_pending_entry_t *entry = &pending->entries[i];
        if (!get_string(&p, end, entry->id, sizeof(entry->id)) || (size_t)(end - p) < 10) return false;
        uint64_t seconds = 0;
        for (unsigned j = 0; j < 8; ++j) seconds |= (uint64_t)*p++ << (j * 8);
        if (seconds > INT64_MAX) return false;
        entry->received_at = (int64_t)seconds;
        entry->state = *p++;
        entry->failure_pending = *p++;
    }
    return p == end;
}
