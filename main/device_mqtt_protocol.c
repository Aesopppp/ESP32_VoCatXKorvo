#include "device_mqtt_protocol.h"
#include "device_handshake.h"
#include "cJSON.h"
#include <string.h>

static bool task_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) > DEVICE_TASK_ID_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("_.:-", *p))) return false;
    return true;
}

bool device_mqtt_parse_task(const char *json, size_t length, device_mqtt_task_t *task)
{
    if (!json || !task || !length || length > DEVICE_TASK_PAYLOAD_MAX) return false;
    memset(task, 0, sizeof(*task));
    /* 协议为扁平对象；在递归解析前限深，拒绝 C 字符串无法表示的 NUL。 */
    bool quoted = false;
    int depth = 0;
    for (size_t i = 0; i < length; ++i) {
        if (!json[i]) return false;
        if (quoted && json[i] == '\\') {
            if (++i == length) return false;
            if (json[i] == 'u' && length - i >= 5 && !memcmp(json + i + 1, "0000", 4)) return false;
        } else if (json[i] == '"') quoted = !quoted;
        else if (!quoted) {
            if (json[i] == '[' || json[i] == ']') return false;
            if (json[i] == '{' && ++depth > 1) return false;
            if (json[i] == '}' && --depth < 0) return false;
        }
    }
    if (quoted || depth) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;
    while (end < json + length && strchr(" \t\r\n", *end)) ++end;
    bool ok = end == json + length && cJSON_IsObject(root);
    for (const cJSON *field = root->child; ok && field; field = field->next) {
        if (!cJSON_IsString(field)) { ok = false; break; }
        for (const cJSON *other = field->next; other; other = other->next)
            if (!strcmp(field->string, other->string)) ok = false;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "task_id");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    const cJSON *fire = cJSON_GetObjectItemCaseSensitive(root, "fire_at");
    int64_t unused_time;
    ok = ok && cJSON_IsString(id) && task_id_valid(id->valuestring) &&
         cJSON_IsString(kind) && !strcmp(kind->valuestring, "reminder") &&
         cJSON_IsString(content) && content->valuestring[0] &&
         cJSON_IsString(fire) && device_handshake_parse_time(fire->valuestring, &unused_time);
    if (ok) {
        strcpy(task->id, id->valuestring);
        memcpy(task->payload, json, length);
        task->payload[length] = 0;
    }
    cJSON_Delete(root);
    return ok;
}

device_mqtt_fragment_result_t device_mqtt_assemble(device_mqtt_assembly_t *s,
    bool allowed, size_t total, size_t offset, const char *data, size_t length)
{
    if (!offset) {
        s->used = s->total = 0;
        if (allowed && total && total <= DEVICE_TASK_PAYLOAD_MAX) s->total = total;
    }
    if (!s->total || total != s->total || offset != s->used || !data || !length ||
        offset > total || length > total - offset || memchr(data, 0, length)) {
        s->used = s->total = 0;
        return DEVICE_MQTT_FRAGMENT_REJECTED;
    }
    memcpy(s->data + offset, data, length);
    s->used += length;
    s->data[s->used] = 0;
    if (s->used < total) return DEVICE_MQTT_FRAGMENT_MORE;
    s->total = 0;
    return DEVICE_MQTT_FRAGMENT_COMPLETE;
}

void device_mqtt_history_init(device_mqtt_history_t *h, const char *device_id)
{
    memset(h, 0, sizeof(*h));
    h->version = 1;
    if (device_handshake_id_valid(device_id)) strcpy(h->device_id, device_id);
}
bool device_mqtt_history_valid(const device_mqtt_history_t *h, const char *device_id)
{
    if (h->version != 1 || h->next >= DEVICE_TASK_HISTORY_SIZE ||
        !memchr(h->device_id, 0, sizeof(h->device_id)) || strcmp(h->device_id, device_id)) return false;
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i)
        if (!memchr(h->ids[i], 0, sizeof(h->ids[i])) ||
            (h->ids[i][0] && !task_id_valid(h->ids[i]))) return false;
    return true;
}
int device_mqtt_history_find(const device_mqtt_history_t *h, const char *id)
{
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i)
        if (id && *id && !strcmp(h->ids[i], id)) return (int)i;
    return -1;
}
unsigned device_mqtt_history_add(device_mqtt_history_t *h, const char *id)
{
    int found = device_mqtt_history_find(h, id);
    if (found >= 0) return (unsigned)found;
    unsigned index = h->next;
    strcpy(h->ids[index], id); /* 调用方只传经过 parse_task 校验的 ID。 */
    h->next = (index + 1) % DEVICE_TASK_HISTORY_SIZE;
    return index;
}
void device_mqtt_retry_failed(device_mqtt_retry_t *s, int connack, int64_t now, uint32_t random)
{
    if (s->failures < 7) ++s->failures;
    if ((connack == 4 || connack == 5) && ++s->auth_failures >= 5) s->blocked = true;
    unsigned delay = s->failures >= 7 ? 60000u : 1000u << (s->failures - 1);
    delay = delay * (80u + random % 41u) / 100u;
    if (delay > 60000u) delay = 60000u;
    s->retry_at_ms = now + delay;
}
void device_mqtt_retry_wake(device_mqtt_retry_t *s)
{
    if (s->blocked) memset(s, 0, sizeof(*s));
}
int device_mqtt_suback(const unsigned char *data, size_t length)
{
    return data && length == 1 && data[0] <= 2 ? data[0] : -1;
}
