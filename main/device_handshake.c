#include "device_handshake.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *device_handshake_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, bytes);
#endif
}

void device_handshake_wipe(void *buffer, size_t bytes)
{
    volatile unsigned char *p = buffer;
    while (bytes--) *p++ = 0;
}

static bool ascii_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool device_handshake_id_valid(const char *value)
{
    if (!value || !value[0] || strlen(value) > 64) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!ascii_alnum(*p) && !strchr("_.:-", *p)) return false;
    return true;
}

static bool text_valid(const char *value, size_t limit)
{
    if (!value || !*value || strlen(value) > limit) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool header_valid(const char *value, size_t limit)
{
    if (!text_valid(value, limit)) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p <= 32 || *p >= 127) return false;
    return true;
}

/* 支持 ASCII 域名/IPv4、可选端口和路径；拒绝用户信息、fragment 和 IPv6 字面量。
 * 基础地址不接受 query，避免拼接固定握手路径后改变请求目标。 */
static bool url_valid(const char *url, const char *plain, const char *tls,
                      bool allow_plain, bool allow_query, bool broker)
{
    if (!header_valid(url, 1024) || strpbrk(url, "\\@#")) return false;
    size_t prefix = strncmp(url, tls, strlen(tls)) == 0 ? strlen(tls) :
                    allow_plain && strncmp(url, plain, strlen(plain)) == 0 ? strlen(plain) : 0;
    if (!prefix) return false;
    const char *p = url + prefix, *host = p;
    while (ascii_alnum((unsigned char)*p) || *p == '-' || *p == '.') ++p;
    if (p == host || !ascii_alnum((unsigned char)*host) ||
        !ascii_alnum((unsigned char)p[-1])) return false;
    if (*p == ':') {
        unsigned port = 0;
        const char *start = ++p;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned)(*p++ - '0');
            if (port > 65535 || p - start > 5) return false;
        }
        if (p == start || port == 0) return false;
    }
    if (broker) return *p == 0;
    if (*p && *p != '/' && !(allow_query && *p == '?')) return false;
    return allow_query || !strchr(p, '?');
}

/* 在 cJSON 递归前限深；NUL（含转义）会让 C 字符串截断，必须拒绝。 */
static bool safe_structure(const char *json, size_t len)
{
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (!c || (quoted && c < 32)) return false;
        if (quoted && c == '\\') {
            if (++i == len) return false;
            if (json[i] == 'u' && len - i >= 5 && memcmp(json + i + 1, "0000", 4) == 0)
                return false;
        } else if (c == '"') quoted = !quoted;
        else if (!quoted) {
            if (c == '{' || c == '[') { if (++depth > 8) return false; }
            else if ((c == '}' || c == ']') && --depth < 0) return false;
        }
    }
    return !quoted && depth == 0;
}

static bool unique_keys(const cJSON *node)
{
    for (const cJSON *child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node)) {
            for (const cJSON *other = child->next; other; other = other->next)
                if (strcmp(child->string, other->string) == 0) return false;
        }
        if (!unique_keys(child)) return false;
    }
    return true;
}

static cJSON *parse_object(const char *json, size_t len, size_t limit)
{
    if (!json || !len || len > limit || !safe_structure(json, len)) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (!root) return NULL;
    while (end < json + len && strchr(" \r\n\t", *end)) ++end;
    if (end != json + len || !cJSON_IsObject(root) || !unique_keys(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool copy_field(const cJSON *root, const char *name, char *dest, size_t size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || !text_valid(value->valuestring, size - 1)) return false;
    strcpy(dest, value->valuestring);
    return true;
}

bool device_handshake_parse_config(const char *json, size_t len,
                                  device_handshake_config_t *output, const char **reason)
{
    memset(output, 0, sizeof(*output));
    *reason = "invalid_config_json";
    cJSON *root = parse_object(json, len, DEVICE_HANDSHAKE_CONFIG_MAX);
    if (!root) return false;
    bool ok = false;
    const char *keys[] = { "enabled", "allow_insecure_http", "server_url", "device_id",
                           "device_model", "psk" };
    for (const cJSON *field = root->child; field; field = field->next) {
        bool known = false;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
            if (strcmp(field->string, keys[i]) == 0) known = true;
        if (!known) { *reason = "unknown_config_field"; goto done; }
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *insecure = cJSON_GetObjectItemCaseSensitive(root, "allow_insecure_http");
    if (!cJSON_IsBool(enabled) || (insecure && !cJSON_IsBool(insecure))) goto done;
    output->enabled = cJSON_IsTrue(enabled);
    output->allow_insecure_http = cJSON_IsTrue(insecure);
    if (!output->enabled) { ok = true; goto done; }
    *reason = "invalid_config_fields";
    if (!copy_field(root, "server_url", output->server_url, sizeof(output->server_url)) ||
        !copy_field(root, "device_id", output->device_id, sizeof(output->device_id)) ||
        !copy_field(root, "psk", output->psk, sizeof(output->psk))) goto done;
    strcpy(output->device_model, "esp32s3");
    if (cJSON_HasObjectItem(root, "device_model") &&
        !copy_field(root, "device_model", output->device_model, sizeof(output->device_model))) goto done;
    if (!device_handshake_id_valid(output->device_id) ||
        !header_valid(output->device_model, 64) || !header_valid(output->psk, 512)) goto done;
    if (!url_valid(output->server_url, "http://", "https://", output->allow_insecure_http,
                   false, false)) { *reason = "invalid_server_url_or_http_disabled"; goto done; }
    size_t n = strlen(output->server_url);
    while (n && output->server_url[n - 1] == '/') output->server_url[--n] = 0;
    ok = true;
done:
    cJSON_Delete(root);
    if (!ok) device_handshake_wipe(output, sizeof(*output));
    else *reason = "ok";
    return ok;
}

static int digits(const char *p, size_t n)
{
    int value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return -1;
        value = value * 10 + p[i] - '0';
    }
    return value;
}

static bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool device_handshake_parse_time(const char *text, int64_t *unix_ms)
{
    if (!text) return false;
    size_t len = strlen(text);
    if (len < 20 || len > 27 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[len - 1] != 'Z') return false;
    int year = digits(text, 4), month = digits(text + 5, 2), day = digits(text + 8, 2);
    int hour = digits(text + 11, 2), minute = digits(text + 14, 2), second = digits(text + 17, 2);
    static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month[month - 1] + (month == 2 && leap_year(year)) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    int millis = 0;
    if (len != 20) {
        if (len < 22 || text[19] != '.' || digits(text + 20, len - 21) < 0) return false;
        for (size_t i = 20; i < 23; ++i) millis = millis * 10 + (i < len - 1 ? text[i] - '0' : 0);
    }
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += 365 + leap_year(y);
    for (int m = 1; m < month; ++m) days += days_in_month[m - 1] + (m == 2 && leap_year(year));
    *unix_ms = (((days + day - 1) * 24 + hour) * 3600 + minute * 60 + second) * 1000 + millis;
    return true;
}

bool device_handshake_parse_response(const char *json, size_t len, const char *device_id,
                                    device_handshake_response_t *output, const char **reason)
{
    memset(output, 0, sizeof(*output));
    *reason = "invalid_response_json";
    cJSON *root = parse_object(json, len, DEVICE_HANDSHAKE_RESPONSE_MAX);
    if (!root) return false;
    bool ok = false;
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    const cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    *reason = "invalid_response_fields";
    if (!device_handshake_id_valid(device_id) || !cJSON_IsObject(mqtt) || !cJSON_IsObject(ws) ||
        !copy_field(root, "server_time", output->server_time, sizeof(output->server_time)) ||
        !device_handshake_parse_time(output->server_time, &output->server_time_ms)) goto done;
#define COPY_FIELD(object, name, member) copy_field(object, name, output->member, sizeof(output->member))
    if (!COPY_FIELD(mqtt, "endpoint", mqtt.endpoint) || !COPY_FIELD(mqtt, "client_id", mqtt.client_id) ||
        !COPY_FIELD(mqtt, "username", mqtt.username) || !COPY_FIELD(mqtt, "password", mqtt.password) ||
        !COPY_FIELD(mqtt, "publish_topic", mqtt.publish_topic) ||
        !COPY_FIELD(mqtt, "subscribe_topic", mqtt.subscribe_topic) ||
        !COPY_FIELD(ws, "url", websocket.url) || !COPY_FIELD(ws, "token", websocket.token)) goto done;
    if (!url_valid(output->mqtt.endpoint, "mqtt://", "mqtts://", false, false, true) ||
        !url_valid(output->websocket.url, "ws://", "wss://", true, true, false)) goto done;
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(ws, "expires_in");
    *reason = "invalid_websocket_expires_in";
    if (!cJSON_IsNumber(expires) || !(expires->valuedouble >= 1 && expires->valuedouble <= UINT32_MAX) ||
        expires->valuedouble != (double)(uint32_t)expires->valuedouble) goto done;
    output->websocket.expires_in = (uint32_t)expires->valuedouble;
    /* 仅比对归属；保存和供后续模块使用的仍是服务端原始字段，不能自己生成凭据。 */
    char expected_topic[129];
    *reason = "device_or_topic_mismatch";
    if (strcmp(output->mqtt.username, device_id) != 0) goto done;
    snprintf(expected_topic, sizeof(expected_topic), "devices/%s/status", device_id);
    if (strcmp(output->mqtt.publish_topic, expected_topic) != 0) goto done;
    snprintf(expected_topic, sizeof(expected_topic), "devices/%s/task", device_id);
    if (strcmp(output->mqtt.subscribe_topic, expected_topic) != 0) goto done;
    *reason = "invalid_firmware_fields";
    if (!cJSON_IsNull(firmware)) {
        if (!cJSON_IsObject(firmware) || !COPY_FIELD(firmware, "version", firmware.version) ||
            !COPY_FIELD(firmware, "url", firmware.url) ||
            !url_valid(output->firmware.url, "http://", "https://", true, true, false)) goto done;
    }
#undef COPY_FIELD
    ok = true;
done:
    cJSON_Delete(root);
    if (!ok) device_handshake_wipe(output, sizeof(*output));
    else *reason = "ok";
    return ok;
}

const char *device_handshake_error_code(const char *json, size_t len)
{
    static const char *codes[] = { "device_credential_invalid", "device_revoked",
                                   "device_disabled", "device_not_found" };
    const char *result = "http_error";
    cJSON *root = parse_object(json, len, DEVICE_HANDSHAKE_RESPONSE_MAX);
    if (!root) return result;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsString(code)) {
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
            if (strcmp(code->valuestring, codes[i]) == 0) result = codes[i];
    }
    cJSON_Delete(root);
    return result;
}

bool device_handshake_retryable(int http_status)
{
    return http_status == 0 || http_status == 408 || http_status == 429 ||
           (http_status >= 500 && http_status <= 599);
}

unsigned device_handshake_retry_delay_ms(unsigned failures)
{
    return failures <= 1 ? 5000 : failures == 2 ? 15000 : 60000;
}

int64_t device_handshake_token_remaining_ms(uint32_t expires_in, int64_t acquired_ms, int64_t now_ms)
{
    if (acquired_ms < 0 || now_ms < acquired_ms) return 0;
    int64_t lifetime_ms = (int64_t)expires_in * 1000;
    int64_t elapsed_ms = now_ms - acquired_ms;
    return elapsed_ms < lifetime_ms ? lifetime_ms - elapsed_ms : 0;
}

void device_handshake_network_changed(device_handshake_schedule_t *state, bool online)
{
    ++state->generation;
    state->online = online;
    state->ready = false;
    /* 保留 blocked 和退避截止时间：抖动不能绕过 401/403/404 或不断重置重试。 */
}

void device_handshake_refresh(device_handshake_schedule_t *state)
{
    ++state->generation;
    state->ready = state->blocked = false;
    state->failures = 0;
    state->retry_at_ms = 0;
}

bool device_handshake_renew(device_handshake_schedule_t *state)
{
    if (state->blocked || !state->ready) return false;
    ++state->generation;
    state->ready = false;
    state->failures = 0;
    state->retry_at_ms = 0;
    return true;
}

bool device_handshake_due(const device_handshake_schedule_t *state, int64_t now_ms)
{
    return state->online && !state->ready && !state->blocked && now_ms >= state->retry_at_ms;
}

bool device_handshake_complete(device_handshake_schedule_t *state, uint32_t generation,
                               int status, bool success, int64_t now_ms)
{
    if (generation != state->generation || !state->online) return false;
    if (success && status == 200) {
        state->ready = true;
        state->failures = 0;
        state->retry_at_ms = 0;
    } else if (device_handshake_retryable(status)) {
        if (state->failures < 3) ++state->failures;
        state->retry_at_ms = now_ms + device_handshake_retry_delay_ms(state->failures);
    } else state->blocked = true;
    return true;
}
