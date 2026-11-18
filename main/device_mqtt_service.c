#include "device_mqtt_service.h"
#include "device_mqtt_storage.h"
#include "wifi_service.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if __has_include("device_mqtt_local_config.h")
#include "device_mqtt_local_config.h"
#endif
#ifndef DEVICE_MQTT_FORWARD_TASK_TO_WS
#define DEVICE_MQTT_FORWARD_TASK_TO_WS 1
#endif
#ifdef DEVICE_MQTT_HAS_CA
extern const char mqtt_ca[] asm("_binary_device_mqtt_ca_pem_start");
#endif

static const char *TAG = "device_mqtt";
#define MQTT_CONTROL_QUEUE_SIZE 48
#define MQTT_STATUS_PERIOD_MS 60000

/* 主任务与 worker 只在短临界段交换配置；绝不持此锁进行网络/Flash 操作。 */
typedef struct {
    bool enabled, configured;
    unsigned epoch, revision;
    device_mqtt_configuration_t config;
} mqtt_settings_t;
static mqtt_settings_t *s_settings;
static SemaphoreHandle_t s_settings_lock;
static QueueHandle_t s_control, s_rx, s_ready, s_results;
static atomic_bool s_callback_overflow;
static atomic_uint s_wake_serial;
static atomic_bool s_dispatch_allowed;
static bool s_started;
static device_mqtt_delivery_t *s_main_delivery; /* 仅主任务使用；最多一个借出对象。 */

typedef struct { int id, msg_id, code, raw_code, tls_error, tls_flags; } mqtt_notice_t;
typedef struct { device_mqtt_delivery_t *tasks; device_mqtt_task_result_t result; } mqtt_result_t;
/* worker 独占；assembly 仅 MQTT 回调写，stop/destroy 等回调退出后复位。 */
typedef struct {
    mqtt_settings_t settings;
    esp_mqtt_client_handle_t client;
    device_mqtt_retry_t retry;
    device_mqtt_assembly_t assembly;
    bool connected, subscribed, subscription_rejected, dirty, pending_dirty, storage_ready;
    int status_id, subscribe_id;
    int ack_ids[DEVICE_TASK_HISTORY_SIZE];
    int64_t ack_sent_at[DEVICE_TASK_HISTORY_SIZE];
    bool ack_pending[DEVICE_TASK_HISTORY_SIZE];
    int64_t connect_deadline, subscription_deadline, next_status, persist_at, next_ack;
    int64_t status_sent_at;
    unsigned seen_wake;
    device_mqtt_history_t history;
    mqtt_pending_store_t pending;
    int failure_ids[DEVICE_TASK_QUEUE_SIZE];
    int64_t failure_sent_at[DEVICE_TASK_QUEUE_SIZE];
    device_mqtt_delivery_t *active; /* 暂借给主任务；worker 只读身份直到结果归还。 */
} mqtt_worker_t;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    mqtt_worker_t *w = arg;
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_DATA) {
        bool allowed = !e->retain && e->qos <= 1 && e->topic && e->topic_len > 0 &&
            (size_t)e->topic_len == strlen(w->settings.config.subscribe_topic) &&
            !memcmp(e->topic, w->settings.config.subscribe_topic, e->topic_len);
        if (e->total_data_len <= 0 || e->current_data_offset < 0 || e->data_len <= 0) {
            w->assembly.total = w->assembly.used = 0;
            return;
        }
        device_mqtt_fragment_result_t result = device_mqtt_assemble(&w->assembly, allowed,
            (size_t)e->total_data_len, (size_t)e->current_data_offset, e->data, (size_t)e->data_len);
        if (result == DEVICE_MQTT_FRAGMENT_COMPLETE) {
            char *payload = device_handshake_alloc(w->assembly.used + 1);
            if (payload) memcpy(payload, w->assembly.data, w->assembly.used + 1);
            if (!payload || xQueueSend(s_rx, &payload, 0) != pdTRUE) {
                free(payload);
                atomic_store(&s_callback_overflow, true);
            }
        } else if (result == DEVICE_MQTT_FRAGMENT_REJECTED && !e->current_data_offset) {
            ESP_LOGW(TAG, "Task rejected: topic/retain/size/fragment invalid");
        }
        return;
    }
    mqtt_notice_t notice = { .id = id, .msg_id = e->msg_id };
    if (id == MQTT_EVENT_SUBSCRIBED) {
        notice.code = device_mqtt_suback((const unsigned char *)e->data, e->data_len);
        notice.raw_code = e->data && e->data_len == 1 ? (unsigned char)e->data[0] : -1;
    } else if (id == MQTT_EVENT_ERROR) {
        if (e->error_handle) {
            if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                notice.code = e->error_handle->connect_return_code;
            notice.tls_error = e->error_handle->esp_tls_stack_err;
            notice.tls_flags = e->error_handle->esp_tls_cert_verify_flags;
        }
    } else if (id != MQTT_EVENT_CONNECTED && id != MQTT_EVENT_DISCONNECTED &&
               id != MQTT_EVENT_PUBLISHED) return;
    if (xQueueSend(s_control, &notice, 0) != pdTRUE)
        atomic_store(&s_callback_overflow, true);
}

static void stop_client(mqtt_worker_t *w)
{
    if (w->client) {
        /* 不在回调停止自身。destroy 完成后才允许替换回调借用的配置。 */
        esp_mqtt_client_stop(w->client);
        esp_mqtt_client_destroy(w->client);
        w->client = NULL;
    }
    w->connected = w->subscribed = w->subscription_rejected = false;
    w->status_id = w->subscribe_id = -1;
    w->connect_deadline = w->subscription_deadline = 0;
    memset(&w->assembly, 0, sizeof(w->assembly));
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i) {
        w->ack_ids[i] = -1;
        if (w->history.ids[i][0]) w->ack_pending[i] = true;
    }
    for (unsigned i = 0; i < DEVICE_TASK_QUEUE_SIZE; ++i) w->failure_ids[i] = -1;
    xQueueReset(s_control);
    atomic_store(&s_callback_overflow, false);
}

static void connection_failed(mqtt_worker_t *w, int connack)
{
    stop_client(w);
    device_mqtt_retry_failed(&w->retry, connack, now_ms(), esp_random());
    if (w->retry.blocked)
        ESP_LOGE(TAG, "CONNACK %d: authentication refused 5 times; waiting for user wake", connack);
    else ESP_LOGW(TAG, "MQTT retry in %lld ms (CONNACK=%d, auth_failures=%u)",
        (long long)(w->retry.retry_at_ms - now_ms()), connack, w->retry.auth_failures);
}

/* 先查询尺寸再分配，兼容 v1 原结构体和 v2 紧凑记录。读取失败不擦除旧键。 */
static esp_err_t load_blob(const char *key, void **data, size_t *length)
{
    *data = NULL;
    *length = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("device_tasks", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, key, NULL, length);
    if (err == ESP_OK) {
        if (!*length || *length > DEVICE_MQTT_STORAGE_MAX) err = ESP_ERR_INVALID_SIZE;
        else if (!(*data = device_handshake_alloc(*length))) err = ESP_ERR_NO_MEM;
        else err = nvs_get_blob(handle, key, *data, length);
    }
    nvs_close(handle);
    return err;
}

static bool load_history(mqtt_worker_t *w)
{
    void *data;
    size_t length;
    esp_err_t err = load_blob("played_v1", &data, &length);
    bool legacy = false;
    bool decoded = err == ESP_OK && device_mqtt_storage_decode_history(data, length, &w->history, &legacy);
    free(data);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        device_mqtt_history_init(&w->history, w->settings.config.username);
        return true;
    }
    if (!decoded) return false;
    if (memchr(w->history.device_id, 0, sizeof(w->history.device_id)) &&
        strcmp(w->history.device_id, w->settings.config.username)) {
        device_mqtt_history_init(&w->history, w->settings.config.username);
        return true;
    }
    if (!device_mqtt_history_valid(&w->history, w->settings.config.username)) return false;
    w->dirty |= legacy; /* 正常 worker 顺序提交；新格式写成功前旧数据仍在。 */
    if (legacy) ESP_LOGI(TAG, "Legacy task history loaded; compact storage migration pending");
    return true;
}

static bool task_id_numeric(const char *id)
{
    if (!id || !*id || strlen(id) > DEVICE_TASK_ID_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

static void pending_init(mqtt_pending_store_t *pending, const char *device_id)
{
    memset(pending, 0, sizeof(*pending));
    pending->version = MQTT_PENDING_VERSION;
    snprintf(pending->device_id, sizeof(pending->device_id), "%s", device_id);
}

static bool pending_valid(const mqtt_pending_store_t *pending, const char *device_id)
{
    if (pending->version != MQTT_PENDING_VERSION || pending->count > DEVICE_TASK_QUEUE_SIZE ||
        !memchr(pending->device_id, 0, sizeof(pending->device_id)) ||
        strcmp(pending->device_id, device_id)) return false;
    for (unsigned i = 0; i < pending->count; ++i) {
        const mqtt_pending_entry_t *entry = &pending->entries[i];
        if (!memchr(entry->id, 0, sizeof(entry->id)) || !task_id_numeric(entry->id) ||
            entry->received_at < 0 || entry->state > TASK_WAIT_NETWORK || entry->failure_pending > 1)
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(entry->id, pending->entries[j].id)) return false;
    }
    return true;
}

static int pending_find(const mqtt_pending_store_t *pending, const char *id)
{
    for (unsigned i = 0; i < pending->count; ++i)
        if (!strcmp(pending->entries[i].id, id)) return (int)i;
    return -1;
}

static void pending_remove(mqtt_worker_t *w, unsigned index)
{
    mqtt_pending_store_t *pending = &w->pending;
    if (index >= pending->count) return;
    --pending->count;
    memmove(&pending->entries[index], &pending->entries[index + 1],
            (pending->count - index) * sizeof(pending->entries[0]));
    memset(&pending->entries[pending->count], 0, sizeof(pending->entries[0]));
    memmove(w->failure_ids + index, w->failure_ids + index + 1,
            (pending->count - index) * sizeof(w->failure_ids[0]));
    memmove(w->failure_sent_at + index, w->failure_sent_at + index + 1,
            (pending->count - index) * sizeof(w->failure_sent_at[0]));
    w->failure_ids[pending->count] = -1;
    w->pending_dirty = true;
}

static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("device_tasks", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        nvs_stats_t stats = {0};
        esp_err_t stats_err = nvs_get_stats(NULL, &stats);
        ESP_LOGE(TAG, "Task NVS write failed: key=%s bytes=%u error=%s available_entries=%d",
                 key, (unsigned)size, esp_err_to_name(err),
                 stats_err == ESP_OK ? (int)stats.available_entries : -1);
    }
    return err;
}

static esp_err_t save_task_store(mqtt_worker_t *w, bool history)
{
    void *data = device_handshake_alloc(DEVICE_MQTT_STORAGE_MAX);
    if (!data) return ESP_ERR_NO_MEM;
    size_t size = history ? device_mqtt_storage_encode_history(&w->history, data, DEVICE_MQTT_STORAGE_MAX) :
                           device_mqtt_storage_encode_pending(&w->pending, data, DEVICE_MQTT_STORAGE_MAX);
    const char *key = history ? "played_v1" : "pending_v1"; /* 保留键名，原位原子替换。 */
    esp_err_t err = size ? save_blob(key, data, size) : ESP_ERR_INVALID_SIZE;
    free(data);
    return err;
}

static bool load_pending(mqtt_worker_t *w)
{
    void *data;
    size_t length;
    esp_err_t err = load_blob("pending_v1", &data, &length);
    bool legacy = false;
    bool decoded = err == ESP_OK && device_mqtt_storage_decode_pending(data, length, &w->pending, &legacy);
    free(data);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pending_init(&w->pending, w->settings.config.username);
        return true;
    }
    if (!decoded) return false;
    if (memchr(w->pending.device_id, 0, sizeof(w->pending.device_id)) &&
        strcmp(w->pending.device_id, w->settings.config.username)) {
        pending_init(&w->pending, w->settings.config.username);
        return true;
    }
    if (!pending_valid(&w->pending, w->settings.config.username)) return false;
    w->pending_dirty |= legacy;
    if (legacy) ESP_LOGI(TAG, "Legacy pending tasks loaded; compact storage migration pending");
    bool reconciled = false;
    for (unsigned i = 0; i < w->pending.count;) {
        if (device_mqtt_history_find(&w->history, w->pending.entries[i].id) >= 0) {
            pending_remove(w, i);
            reconciled = true;
        } else {
            if (w->pending.entries[i].state == TASK_WAIT_NETWORK) {
                w->pending.entries[i].state = TASK_READY;
                reconciled = true;
            }
            ++i;
        }
    }
    if (reconciled) {
        w->pending_dirty = true;
    }
    /* 有效数据已读入。交给 persist_state 重试迁移/对账，不能因一次写失败永久锁死恢复。 */
    return true;
}

static void persist_state(mqtt_worker_t *w)
{
    if ((!w->dirty && !w->pending_dirty) || now_ms() < w->persist_at) return;
    esp_err_t err = ESP_OK;
    /* 先落成功历史，再移除待播项；两次提交之间掉电时，启动对账可剔除旧待播副本。 */
    if (w->dirty) {
        err = save_task_store(w, true);
        if (err == ESP_OK) w->dirty = false;
    }
    if (err == ESP_OK && w->pending_dirty) {
        err = save_task_store(w, false);
        if (err == ESP_OK) {
            w->pending_dirty = false;
            ESP_LOGI(TAG, "Task queue persisted: count=%u", (unsigned)w->pending.count);
        }
    }
    if (err == ESP_OK) return;
    w->persist_at = now_ms() + 5000;
    ESP_LOGE(TAG, "Task state save failed: %s; ACK and next playback held", esp_err_to_name(err));
}

static int enqueue_task_ack(mqtt_worker_t *w, const char *const *ids, unsigned count,
                            bool audio_received)
{
    if (!w->connected || !w->client || !count || count > DEVICE_TASK_QUEUE_SIZE) return -1;
    char topic[96], payload[1200];
    int used = snprintf(topic, sizeof(topic), "devices/%s/ack", w->settings.config.username);
    if (used <= 0 || (size_t)used >= sizeof(topic)) return -1;
    used = snprintf(payload, sizeof(payload), "{\"task_ids\":[");
    for (unsigned i = 0; i < count && used > 0 && (size_t)used < sizeof(payload); ++i) {
        if (!task_id_numeric(ids[i])) return -1;
        int n = snprintf(payload + used, sizeof(payload) - (size_t)used, "%s\"%s\"",
                         i ? "," : "", ids[i]);
        if (n <= 0 || (size_t)n >= sizeof(payload) - (size_t)used) return -1;
        used += n;
    }
    int n = snprintf(payload + used, sizeof(payload) - (size_t)used,
                     "],\"audio_received\":%s}", audio_received ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(payload) - (size_t)used) return -1;
    int msg_id = esp_mqtt_client_enqueue(w->client, topic, payload, 0, 1, 0, true);
    if (msg_id < 0) ESP_LOGW(TAG, "Task ACK enqueue failed: count=%u audio_received=%d", count, audio_received);
    else ESP_LOGI(TAG, "Task ACK queued: count=%u audio_received=%d", count, audio_received);
    return msg_id;
}

static void consume_result(mqtt_worker_t *w)
{
    mqtt_result_t event;
    if (xQueueReceive(s_results, &event, 0) != pdTRUE) return;
    if (event.tasks != w->active) {
        ESP_LOGE(TAG, "Unexpected task result; no ACK");
        return;
    }
    device_mqtt_delivery_t *tasks = event.tasks;
    /* 身份切换后旧会话不得替新设备删除任务或生成回执。 */
    if (w->storage_ready && !strcmp(tasks->device_id, w->history.device_id)) {
        for (unsigned i = 0; i < tasks->count; ++i) {
            int index = pending_find(&w->pending, tasks->task_ids[i]);
            if (event.result == DEVICE_TASK_AUDIO_RECEIVED) {
                unsigned slot = device_mqtt_history_add(&w->history, tasks->task_ids[i]);
                w->ack_pending[slot] = true;
                w->ack_ids[slot] = -1;
                w->dirty = true;
                if (index >= 0) pending_remove(w, (unsigned)index);
            } else if (index >= 0) {
                mqtt_pending_entry_t *entry = &w->pending.entries[index];
                entry->state = event.result == DEVICE_TASK_NETWORK_WAIT ?
                    TASK_WAIT_NETWORK : TASK_WAIT_PLATFORM;
                entry->failure_pending = 1;
                w->failure_ids[index] = -1;
                w->pending_dirty = true;
            }
        }
        w->persist_at = 0;
        ESP_LOGI(TAG, "Reminder result: task_count=%u audio_received=%d network_wait=%d",
                 tasks->count, event.result == DEVICE_TASK_AUDIO_RECEIVED,
                 event.result == DEVICE_TASK_NETWORK_WAIT);
    }
    free(tasks);
    w->active = NULL;
}

static void consume_task(mqtt_worker_t *w, char *payload)
{
    device_mqtt_task_t *message = device_handshake_alloc(sizeof(*message));
    if (!message || !device_mqtt_parse_task(payload, strlen(payload), message)) {
        ESP_LOGW(TAG, "Invalid task JSON/schema or allocation failed; no playback/ACK");
        free(message);
        return;
    }
    if (!w->storage_ready) {
        ESP_LOGE(TAG, "Task storage unavailable; not accepting id=%s", message->id);
        free(message);
        return;
    }
    int played = device_mqtt_history_find(&w->history, message->id);
    if (played >= 0) {
        w->ack_pending[played] = true;
        ESP_LOGI(TAG, "Task already confirmed: id=%s; ACK again without a new session", message->id);
        free(message);
        return;
    }
    int index = pending_find(&w->pending, message->id);
    if (index >= 0) {
        mqtt_pending_entry_t *entry = &w->pending.entries[index];
        /* 失败后仅新的平台投递可以重开；普通 QoS 重复/活跃任务不重新建会话。
         * 若失败 PUBACK 还未到，先保留新意图，ACK 落盘后才允许播放。 */
        if (entry->state != TASK_READY) {
            entry->state = TASK_READY;
            w->pending_dirty = true;
            w->persist_at = 0;
            ESP_LOGI(TAG, "Task redelivered by platform: id=%s", message->id);
        } else ESP_LOGI(TAG, "Task duplicate ignored: id=%s", message->id);
        free(message);
        return;
    }
    if (w->pending.count == DEVICE_TASK_QUEUE_SIZE) {
        ESP_LOGW(TAG, "Task queue full: id=%s; no success ACK", message->id);
        free(message);
        return;
    }
    unsigned slot = w->pending.count++;
    mqtt_pending_entry_t *entry = &w->pending.entries[slot];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->id, message->id);
    time_t received_at = time(NULL);
    entry->received_at = received_at > 0 ? (int64_t)received_at : 0;
    w->failure_ids[slot] = -1;
    w->pending_dirty = true;
    w->persist_at = 0;
    /* worker 批量提交后才能发给主任务。失败保持 RAM 副本并定时重试存储。 */
    ESP_LOGI(TAG, "Task queued: id=%s count=%u voice_adapter=%d; persistence pending", message->id,
             (unsigned)w->pending.count, DEVICE_MQTT_FORWARD_TASK_TO_WS);
    free(message);
}

static void publish_status(mqtt_worker_t *w)
{
    if (!w->connected || now_ms() < w->next_status) return;
    if (w->status_id >= 0) {
        if (now_ms() - w->status_sent_at >= 90000) connection_failed(w, 0);
        return; /* 至多一条待 PUBACK 的 status，断网不能无限堆积。 */
    }
    char ip[16] = {0}, payload[320];
    wifi_service_get_ip_str(ip, sizeof(ip));
    cJSON *root = cJSON_CreateObject();
    if (!root) { w->next_status = now_ms() + 1000; return; }
    bool ok = cJSON_AddStringToObject(root, "firmware_version", esp_app_get_description()->version) &&
        cJSON_AddStringToObject(root, "board", "esp-vocat-v1.2") &&
        cJSON_AddStringToObject(root, "ip", ip) &&
        cJSON_AddNumberToObject(root, "rssi", wifi_service_get_rssi()) &&
        cJSON_PrintPreallocated(root, payload, sizeof(payload), false);
    cJSON_Delete(root);
    int id = ok ? esp_mqtt_client_enqueue(w->client, w->settings.config.publish_topic,
                                         payload, 0, 1, 0, true) : -1;
    if (id >= 0) {
        w->status_id = id;
        w->status_sent_at = now_ms();
        w->next_status = now_ms() + MQTT_STATUS_PERIOD_MS;
        /* 只在周期上报时记录，不打印凭据或业务正文。enqueue 成功不代表 broker 已接收。 */
        ESP_LOGI(TAG, "Status queued: msg_id=%d topic=%s; awaiting PUBACK", id,
                 w->settings.config.publish_topic);
    } else w->next_status = now_ms() + 1000;
}

static void publish_ack(mqtt_worker_t *w)
{
    if (!w->connected || w->dirty || w->pending_dirty || !w->storage_ready || now_ms() < w->next_ack) return;
    w->next_ack = now_ms() + 100;
    const char *ids[DEVICE_TASK_QUEUE_SIZE];
    unsigned slots[DEVICE_TASK_QUEUE_SIZE], count = 0;
    /* 失败回执保留在 NVS，掉线/重启后重发；PUBACK 后释放失败记录。
     * 回执确认期间的新平台投递保留为 READY，不能被迟到 PUBACK 删除。 */
    for (unsigned i = 0; i < w->pending.count; ++i) {
        if (!w->pending.entries[i].failure_pending) continue;
        if (w->failure_ids[i] >= 0 && now_ms() - w->failure_sent_at[i] >= 45000)
            w->failure_ids[i] = -1;
        if (w->failure_ids[i] >= 0) continue;
        ids[count] = w->pending.entries[i].id;
        slots[count++] = i;
    }
    if (count) {
        int msg_id = enqueue_task_ack(w, ids, count, false);
        if (msg_id >= 0) for (unsigned i = 0; i < count; ++i) {
            w->failure_ids[slots[i]] = msg_id;
            w->failure_sent_at[slots[i]] = now_ms();
        }
        return;
    }
    for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i) {
        /* 客户端 outbox 有过期上限；不能把已过期的 msg_id 当成永久在途。
         * 重新发送同 task_id 是幂等的，迟到的旧 PUBACK 不会匹配新 msg_id。 */
        if (w->ack_pending[i] && w->ack_ids[i] >= 0 &&
            now_ms() - w->ack_sent_at[i] >= 45000) w->ack_ids[i] = -1;
        if (!w->ack_pending[i] || w->ack_ids[i] >= 0 || !w->history.ids[i][0]) continue;
        if (!task_id_numeric(w->history.ids[i])) {
            w->ack_pending[i] = false;
            ESP_LOGW(TAG, "Skipping legacy nonnumeric task ID in success ACK history");
            continue;
        }
        ids[count] = w->history.ids[i];
        slots[count++] = i;
        if (count == DEVICE_TASK_QUEUE_SIZE) break;
    }
    if (!count) return;
    int msg_id = enqueue_task_ack(w, ids, count, true);
    if (msg_id < 0) return;
    for (unsigned i = 0; i < count; ++i) {
        w->ack_ids[slots[i]] = msg_id;
        w->ack_sent_at[slots[i]] = now_ms();
    }
}

static void consume_notice(mqtt_worker_t *w, const mqtt_notice_t *event)
{
    if (!w->client) return;
    switch (event->id) {
    case MQTT_EVENT_CONNECTED:
        w->connected = true;
        w->connect_deadline = 0;
        w->retry.auth_failures = 0;
        ESP_LOGI(TAG, "MQTT connected: client_id=%s username=%s (3.1.1, TLS)",
                 w->settings.config.client_id, w->settings.config.username);
        w->subscribe_id = esp_mqtt_client_subscribe(w->client, w->settings.config.subscribe_topic, 1);
        w->subscription_deadline = now_ms() + 10000;
        w->next_status = 0;
        if (w->subscribe_id < 0) connection_failed(w, 0);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->msg_id != w->subscribe_id) break;
        w->subscription_deadline = 0;
        if (event->code < 0) {
            w->subscribed = false;
            w->subscription_rejected = true;
            ESP_LOGE(TAG, "SUBACK rejected: code=%d topic=%s; connection retained, tasks unavailable",
                     event->raw_code, w->settings.config.subscribe_topic);
        } else {
            w->subscription_rejected = false;
            w->subscribed = true;
            ESP_LOGI(TAG, "SUBACK granted QoS=%d: topic=%s", event->code, w->settings.config.subscribe_topic);
            if (event->code != 1)
                ESP_LOGW(TAG, "Broker granted QoS %d despite requested QoS 1", event->code);
        }
        break;
    case MQTT_EVENT_PUBLISHED: {
        if (event->msg_id == w->status_id) {
            ESP_LOGI(TAG, "Status PUBACK: msg_id=%d; next report in %lld ms", event->msg_id,
                     (long long)(w->next_status > now_ms() ? w->next_status - now_ms() : 0));
            w->status_id = -1;
            w->retry.failures = 0;
        }
        unsigned acked = 0;
        for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i) {
            if (w->ack_ids[i] != event->msg_id) continue;
            w->ack_ids[i] = -1;
            w->ack_pending[i] = false;
            ++acked;
        }
        if (acked) ESP_LOGI(TAG, "Playback ACK PUBACK: msg_id=%d task_count=%u", event->msg_id, acked);
        unsigned failed = 0;
        for (unsigned i = 0; i < w->pending.count;) {
            if (w->failure_ids[i] != event->msg_id) { ++i; continue; }
            w->failure_ids[i] = -1;
            w->pending.entries[i].failure_pending = 0;
            w->pending_dirty = true;
            ++failed;
            if (w->pending.entries[i].state == TASK_WAIT_PLATFORM) pending_remove(w, i);
            else ++i;
        }
        if (failed) {
            w->persist_at = 0;
            ESP_LOGI(TAG, "Failure ACK PUBACK: msg_id=%d task_count=%u", event->msg_id, failed);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error: CONNACK=%d tls=%d verify_flags=0x%x (credentials redacted)",
                 event->code, event->tls_error, (unsigned)event->tls_flags);
        connection_failed(w, event->code);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected; check network and platform ACL (topics came from handshake)");
        connection_failed(w, 0);
        break;
    default: break;
    }
}

static void start_client(mqtt_worker_t *w)
{
#ifdef DEVICE_MQTT_HAS_CA
    const char *certificate = mqtt_ca;
#else
    const char *certificate = NULL;
#endif
    if (!certificate) return; /* init 已拒绝启动；保留完整代码的编译检查。 */
    const device_mqtt_configuration_t *c = &w->settings.config;
    esp_mqtt_client_config_t config = {
        .broker.address.uri = c->endpoint,
        .broker.verification.certificate = certificate,
        .broker.verification.skip_cert_common_name_check = false,
        .credentials.client_id = c->client_id,
        .credentials.username = c->username,
        .credentials.authentication.password = c->password,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = 10000,
        .task.priority = 3, .task.stack_size = 6144,
        .buffer.size = 1024, .buffer.out_size = 1024,
        .outbox.limit = 16384,
    };
    w->client = esp_mqtt_client_init(&config);
    if (!w->client ||
        esp_mqtt_client_register_event(w->client, ESP_EVENT_ANY_ID, mqtt_event, w) != ESP_OK ||
        esp_mqtt_client_start(w->client) != ESP_OK) {
        connection_failed(w, 0);
        return;
    }
    w->connect_deadline = now_ms() + 25000;
}

static void mqtt_worker_step(mqtt_worker_t *w, mqtt_settings_t *next)
{
    consume_result(w);
    persist_state(w);
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    bool changed = w->settings.revision != s_settings->revision;
    if (changed) *next = *s_settings;
    xSemaphoreGive(s_settings_lock);
    if (changed) {
        stop_client(w);
        /* 旧连接已收到的消息先归入旧身份，之后才替换回调借用的凭据。 */
        char *payload;
        while (xQueueReceive(s_rx, &payload, 0) == pdTRUE) {
            if (w->settings.configured) consume_task(w, payload);
            free(payload);
        }
        bool new_identity = next->configured &&
            (!memchr(w->history.device_id, 0, sizeof(w->history.device_id)) ||
             strcmp(w->history.device_id, next->config.username));
        bool online_again = next->enabled && next->configured &&
            (!w->settings.enabled || !w->settings.configured || next->epoch != w->settings.epoch);
        device_handshake_wipe(&w->settings, sizeof(w->settings));
        w->settings = *next;
        device_handshake_wipe(next, sizeof(*next));
        if (new_identity) {
            memset(&w->pending, 0, sizeof(w->pending));
            w->dirty = w->pending_dirty = false;
            w->storage_ready = load_history(w) && load_pending(w);
            if (!w->storage_ready)
                ESP_LOGE(TAG, "Task storage unavailable/corrupt; playback blocked to avoid loss or duplicates");
            memset(w->ack_pending, 0, sizeof(w->ack_pending));
            for (unsigned i = 0; i < DEVICE_TASK_HISTORY_SIZE; ++i)
                if (w->storage_ready && w->history.ids[i][0]) w->ack_pending[i] = true;
        }
        if (online_again && w->storage_ready) {
            for (unsigned i = 0; i < w->pending.count; ++i)
                if (w->pending.entries[i].state == TASK_WAIT_NETWORK) {
                    w->pending.entries[i].state = TASK_READY;
                    w->pending_dirty = true;
                    w->persist_at = 0;
                }
        }
        /* 短暂掉网不能解除鉴权熔断，只有真实用户唤醒或重启可以。 */
    }
    unsigned wake = atomic_load(&s_wake_serial);
    if (wake != w->seen_wake) { w->seen_wake = wake; device_mqtt_retry_wake(&w->retry); }
    mqtt_notice_t event;
    while (xQueueReceive(s_control, &event, 0) == pdTRUE) consume_notice(w, &event);
    char *payload;
    while (xQueueReceive(s_rx, &payload, 0) == pdTRUE) {
        if (w->settings.configured) consume_task(w, payload);
        free(payload);
    }
    if (atomic_exchange(&s_callback_overflow, false)) {
        ESP_LOGE(TAG, "MQTT receive/control queue full; resynchronizing (no playback ACK)");
        connection_failed(w, 0);
    }
    if ((w->connect_deadline && now_ms() >= w->connect_deadline) ||
        (w->subscription_deadline && now_ms() >= w->subscription_deadline))
        connection_failed(w, 0);
    if (w->settings.enabled && w->settings.configured && !w->client &&
        !w->retry.blocked && now_ms() >= w->retry.retry_at_ms) start_client(w);
    persist_state(w);
    publish_status(w);
    publish_ack(w);
    if (!atomic_load(&s_dispatch_allowed) && w->active) {
        /* 空闲快照之后用户可能先唤醒。只回收尚在队列、未借出的批次，
         * 待通话结束再与新到任务合并；主任务已领取的对象不能改动。 */
        device_mqtt_delivery_t *unclaimed = NULL;
        if (xQueueReceive(s_ready, &unclaimed, 0) == pdTRUE) {
            configASSERT(unclaimed == w->active);
            free(unclaimed);
            w->active = NULL;
        }
    }
    if (DEVICE_MQTT_FORWARD_TASK_TO_WS && atomic_load(&s_dispatch_allowed) &&
        w->settings.enabled && w->settings.configured && w->storage_ready &&
        !w->dirty && !w->pending_dirty && !w->active && w->pending.count) {
        bool ready = false;
        for (unsigned i = 0; i < w->pending.count; ++i)
            if (w->pending.entries[i].state == TASK_READY && !w->pending.entries[i].failure_pending)
                ready = true;
        device_mqtt_delivery_t *tasks = ready ? device_handshake_alloc(sizeof(*tasks)) : NULL;
        if (tasks) {
            strcpy(tasks->device_id, w->pending.device_id);
            for (unsigned i = 0; i < w->pending.count; ++i) {
                const mqtt_pending_entry_t *entry = &w->pending.entries[i];
                if (entry->state == TASK_READY && !entry->failure_pending)
                    strcpy(tasks->task_ids[tasks->count++], entry->id);
            }
            if (tasks->count && xQueueSend(s_ready, &tasks, 0) == pdTRUE) {
                w->active = tasks;
                ESP_LOGI(TAG, "Reminder batch ready: task_count=%u", tasks->count);
            } else free(tasks);
        }
    }
}

static void mqtt_worker(void *arg)
{
    mqtt_worker_t *w = arg;
    /* scratch 在初始化时一次分配；不让已宣布 ready 的 worker 因迟到分配失败退出。 */
    mqtt_settings_t *next = (mqtt_settings_t *)(w + 1);
    while (1) {
        mqtt_worker_step(w, next);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t device_mqtt_service_init(void)
{
    if (s_started) return ESP_OK;
#ifndef DEVICE_MQTT_HAS_CA
    ESP_LOGW(TAG, "MQTT disabled: supply main/certs/device_mqtt_ca.pem and rebuild; TLS verification required");
    return ESP_ERR_NOT_FOUND;
#endif
    s_settings = device_handshake_alloc(sizeof(*s_settings));
    mqtt_worker_t *worker = device_handshake_alloc(sizeof(*worker) + sizeof(mqtt_settings_t));
    s_settings_lock = xSemaphoreCreateMutex();
    s_control = xQueueCreate(MQTT_CONTROL_QUEUE_SIZE, sizeof(mqtt_notice_t));
    s_rx = xQueueCreate(DEVICE_TASK_QUEUE_SIZE, sizeof(char *));
    s_ready = xQueueCreate(1, sizeof(device_mqtt_delivery_t *));
    s_results = xQueueCreate(1, sizeof(mqtt_result_t));
    if (!s_settings || !worker || !s_settings_lock || !s_control || !s_rx || !s_ready || !s_results ||
        xTaskCreatePinnedToCore(mqtt_worker, "device_mqtt", 6144, worker, 2, NULL, 0) != pdPASS) {
        free(worker); free(s_settings); s_settings = NULL;
        if (s_settings_lock) vSemaphoreDelete(s_settings_lock);
        if (s_control) vQueueDelete(s_control);
        if (s_rx) vQueueDelete(s_rx);
        if (s_ready) vQueueDelete(s_ready);
        if (s_results) vQueueDelete(s_results);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "MQTT service ready; voice forwarding=%d, pending=%u, persistent history=%u",
             DEVICE_MQTT_FORWARD_TASK_TO_WS, DEVICE_TASK_QUEUE_SIZE, DEVICE_TASK_HISTORY_SIZE);
    return ESP_OK;
}

void device_mqtt_service_poll(bool enabled, unsigned epoch, const device_handshake_response_t *configuration)
{
    if (!s_started) return;
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    if (s_settings->epoch != epoch || s_settings->enabled != enabled) {
        s_settings->epoch = epoch;
        s_settings->enabled = enabled;
        s_settings->configured = false;
        device_handshake_wipe(&s_settings->config, sizeof(s_settings->config));
        ++s_settings->revision;
    }
    /* WS token 续期时 NULL 不等于 MQTT 失效；网络/身份变化由 epoch 单独标记。 */
    if (enabled && configuration &&
        (!s_settings->configured || memcmp(&s_settings->config, &configuration->mqtt, sizeof(s_settings->config)))) {
        s_settings->config = configuration->mqtt;
        s_settings->configured = true;
        ++s_settings->revision;
    }
    xSemaphoreGive(s_settings_lock);
}

void device_mqtt_service_on_wake(void) { atomic_fetch_add(&s_wake_serial, 1); }
void device_mqtt_service_set_dispatch_allowed(bool allowed) { atomic_store(&s_dispatch_allowed, allowed); }
bool device_mqtt_service_voice_enabled(void) { return s_started && DEVICE_MQTT_FORWARD_TASK_TO_WS; }

device_mqtt_delivery_t *device_mqtt_service_take_tasks(void)
{
    if (!s_started || s_main_delivery) return NULL;
    if (xQueueReceive(s_ready, &s_main_delivery, 0) != pdTRUE) return NULL;
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    bool current = s_settings->enabled && s_settings->configured &&
                   !strcmp(s_main_delivery->device_id, s_settings->config.username);
    xSemaphoreGive(s_settings_lock);
    if (!current) { device_mqtt_service_finish_tasks(s_main_delivery, DEVICE_TASK_NETWORK_WAIT); return NULL; }
    return s_main_delivery;
}

void device_mqtt_service_finish_tasks(device_mqtt_delivery_t *tasks, device_mqtt_task_result_t result)
{
    if (!tasks || tasks != s_main_delivery) return;
    mqtt_result_t event = { .tasks = tasks, .result = result };
    /* 前一结果未消费就不可能借出下一对象，队列必有空位。 */
    BaseType_t sent = xQueueSend(s_results, &event, 0);
    configASSERT(sent == pdTRUE);
    s_main_delivery = NULL;
}
