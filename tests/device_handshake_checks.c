/* 主机回归直接运行生产协议/HTTP 交换代码；假传输不连接平台、不使用真实凭据。 */
#include "../main/device_handshake_http.h"
#include "../main/device_handshake_local_config.example.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

static const char *CONFIG_JSON =
    "{\"enabled\":true,\"server_url\":\"https://example.com/base/\","
    "\"device_id\":\"dev-001\",\"psk\":\"test-only-psk\"}";
static const char *RESPONSE_JSON =
    "{\"server_time\":\"2026-11-03T02:00:00.000Z\",\"firmware\":null,"
    "\"mqtt\":{\"endpoint\":\"mqtts://10.0.0.10:8883\","
    "\"client_id\":\"GID_esp32s3@@@dev-001@@@dev-001\",\"username\":\"dev-001\","
    "\"password\":\"server-derived-secret\",\"publish_topic\":\"devices/dev-001/status\","
    "\"subscribe_topic\":\"devices/dev-001/task\"},"
    "\"websocket\":{\"url\":\"wss://gateway.example.com/v1/realtime\","
    "\"token\":\"test-only-token\",\"expires_in\":3600}}";

static bool parse_config(const char *json, device_handshake_config_t *config)
{
    const char *reason = NULL;
    bool ok = device_handshake_parse_config(json, strlen(json), config, &reason);
    CHECK(reason != NULL);
    return ok;
}

static void check_config_change(const char *key, const char *value, bool accepted)
{
    cJSON *root = cJSON_Parse(CONFIG_JSON);
    CHECK(root != NULL);
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, value);
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    device_handshake_config_t config;
    CHECK(parse_config(json, &config) == accepted);
    if (!accepted) CHECK(!config.enabled && config.psk[0] == 0);
    free(json);
    cJSON_Delete(root);
}

static void config_checks(void)
{
    device_handshake_config_t config;
    CHECK(parse_config(DEVICE_HANDSHAKE_LOCAL_JSON, &config) && !config.enabled);
    CHECK(parse_config("{\"enabled\":false}", &config) && !config.enabled);
    CHECK(parse_config(CONFIG_JSON, &config) && config.enabled);
    CHECK(!config.allow_insecure_http);
    CHECK(strcmp(config.server_url, "https://example.com/base") == 0);
    CHECK(strcmp(config.device_model, "esp32s3") == 0);
    CHECK(strcmp(config.psk, "test-only-psk") == 0);
    const char *bad[] = {
        "{}", "[]", "{\"enabled\":1}", "{\"enabled\":true}",
        "{\"enabled\":false,\"enabled\":true}", "{\"enabled\":false}x",
        "{\"enabled\":false,\"psk\":\"a\\u0000b\"}",
        "{\"enabled\":false,\"enable\":true}",
        "{\"enabled\":false,\"allow_insecure_http\":\"true\"}",
        "{\"enabled\":false,\"psk\":[[[[[[[[1]]]]]]]]}",
        "{\"enabled\":false,\"psk\":\"line\nbreak\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) CHECK(!parse_config(bad[i], &config));
    const char *urls[] = { "http://example.com", "https://u:p@example.com", "https://example.com?x=1",
        "https://example.com#part", "https://example.com:0", "https://example.com:65536",
        "https://example.com:\n443", "https://example.com\\evil", "https://", "wss://example.com",
        "https://[::1]", "https://-host.com", "https://example.com:", "https://a:000001" };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) check_config_change("server_url", urls[i], false);
    check_config_change("server_url", "https://10.0.0.10:8092/proxy/", true);
    check_config_change("server_url", "https://a:65535", true);
    check_config_change("psk", "x\r\nInjected: y", false);
    check_config_change("device_model", "esp32 s3", false);
    check_config_change("device_id", "dev_7k2m9x4p0q3z", true);
    check_config_change("device_id", "dev/001", false);
    check_config_change("device_id", "dev#001", false);
    check_config_change("device_id", "", false);
    check_config_change("device_id", "DEV-001", true);
    char boundary[514];
    memset(boundary, 'x', sizeof(boundary));
    boundary[64] = 0;
    check_config_change("device_id", boundary, true);
    boundary[64] = 'x'; boundary[65] = 0;
    check_config_change("device_id", boundary, false);
    memset(boundary, 'x', sizeof(boundary)); boundary[512] = 0;
    check_config_change("psk", boundary, true);
    boundary[512] = 'x'; boundary[513] = 0;
    check_config_change("psk", boundary, false);
    CHECK(parse_config("{\"enabled\":true,\"server_url\":\"http://10.0.0.10:8092\","
        "\"device_id\":\"dev-001\",\"psk\":\"test\",\"allow_insecure_http\":true}", &config));
    const char *reason;
    const char raw_nul[] = "{\"enabled\":false}\0{}";
    CHECK(!device_handshake_parse_config(raw_nul, sizeof(raw_nul) - 1, &config, &reason));
    char large[DEVICE_HANDSHAKE_CONFIG_MAX + 2];
    memset(large, ' ', sizeof(large)); memcpy(large, "{\"enabled\":false}", 17);
    CHECK(device_handshake_parse_config(large, DEVICE_HANDSHAKE_CONFIG_MAX, &config, &reason));
    CHECK(!device_handshake_parse_config(large, DEVICE_HANDSHAKE_CONFIG_MAX + 1, &config, &reason));
}

static void time_checks(void)
{
    int64_t value = -1, fraction;
    CHECK(device_handshake_parse_time("1970-01-01T00:00:00Z", &value) && value == 0);
    CHECK(device_handshake_parse_time("2026-11-03T02:00:00.000Z", &value) && value == 1793671200000LL);
    CHECK(device_handshake_parse_time("2026-11-03T02:00:00.1Z", &fraction) && fraction == value + 100);
    CHECK(device_handshake_parse_time("2026-11-03T02:00:00.12Z", &fraction) && fraction == value + 120);
    CHECK(device_handshake_parse_time("2026-11-03T02:00:00.123456Z", &fraction) && fraction == value + 123);
    CHECK(device_handshake_parse_time("2000-02-29T00:00:00Z", &value) && value == 951782400000LL);
    CHECK(device_handshake_parse_time("2024-02-29T23:59:59Z", &value));
    const char *bad[] = { "2026-02-29T00:00:00Z", "2100-02-29T00:00:00Z",
        "2026-13-03T02:00:00Z", "2026-00-03T02:00:00Z", "2026-11-00T02:00:00Z",
        "2026-04-31T02:00:00Z", "2026-11-03T24:00:00Z", "2026-11-03T02:60:00Z",
        "2026-11-03T02:00:60Z", "2026-11-03T02:00:00.Z", "2026-11-03T02:00:00.xZ",
        "2026-11-03T02:00:00.1234567Z", "2026-11-03T10:00:00+08:00", "1969-12-31T23:59:59Z", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(!device_handshake_parse_time(bad[i], &value));
}

static bool parse_response(const char *json, const char *id, device_handshake_response_t *response)
{
    const char *reason;
    bool ok = device_handshake_parse_response(json, strlen(json), id, response, &reason);
    CHECK(reason != NULL);
    if (!ok) CHECK(response->mqtt.password[0] == 0 && response->websocket.token[0] == 0);
    return ok;
}

static void check_response_change(const char *section, const char *key, const char *value, bool accepted)
{
    cJSON *root = cJSON_Parse(RESPONSE_JSON);
    CHECK(root != NULL);
    cJSON *object = section ? cJSON_GetObjectItemCaseSensitive(root, section) : root;
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    if (value) cJSON_AddStringToObject(object, key, value);
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    device_handshake_response_t response;
    CHECK(parse_response(json, "dev-001", &response) == accepted);
    free(json); cJSON_Delete(root);
}

static void response_checks(void)
{
    device_handshake_response_t response;
    CHECK(parse_response(RESPONSE_JSON, "dev-001", &response));
    CHECK(strcmp(response.mqtt.client_id, "GID_esp32s3@@@dev-001@@@dev-001") == 0);
    CHECK(strcmp(response.mqtt.password, "server-derived-secret") == 0);
    CHECK(strcmp(response.mqtt.endpoint, "mqtts://10.0.0.10:8883") == 0);
    CHECK(strcmp(response.websocket.token, "test-only-token") == 0);
    CHECK(response.websocket.expires_in == 3600);
    CHECK(!response.firmware.url[0] && !response.firmware.version[0]);
    CHECK(!parse_response(RESPONSE_JSON, "DEV-001", &response));
    const char *fields[] = { "endpoint", "client_id", "username", "password", "publish_topic", "subscribe_topic" };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        check_response_change("mqtt", fields[i], NULL, false);
        check_response_change("mqtt", fields[i], "", false);
    }
    check_response_change("websocket", "url", NULL, false);
    check_response_change("websocket", "token", NULL, false);
    check_response_change("websocket", "expires_in", NULL, false);
    check_response_change("websocket", "expires_in", "3600", false);
    check_response_change(NULL, "server_time", NULL, false);
    check_response_change(NULL, "firmware", NULL, false);
    check_response_change("mqtt", "username", "dev-other", false);
    check_response_change("mqtt", "publish_topic", "devices/dev-other/status", false);
    check_response_change("mqtt", "subscribe_topic", "devices/+/task", false);
    check_response_change("mqtt", "subscribe_topic", "devices/dev-001/task ", false);
    check_response_change("mqtt", "endpoint", "mqtts://broker.example.com:8883", true);
    check_response_change("mqtt", "endpoint", "mqtt://broker.example.com:1883", false);
    check_response_change("mqtt", "endpoint", "mqtt://user:pass@broker", false);
    check_response_change("mqtt", "endpoint", "mqtt://broker/path", false);
    check_response_change("websocket", "url", "ws://10.0.0.10:8091/v1/realtime?test=1", true);
    check_response_change("websocket", "url", "file:///secret", false);
    char long_token[2050]; memset(long_token, 'a', sizeof(long_token)); long_token[2048] = 0;
    check_response_change("websocket", "token", long_token, true);
    long_token[2048] = 'a'; long_token[2049] = 0;
    check_response_change("websocket", "token", long_token, false);
    cJSON *root = cJSON_Parse(RESPONSE_JSON);
    cJSON_DeleteItemFromObjectCaseSensitive(root, "firmware");
    cJSON *firmware = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(firmware, "version", "2.0");
    cJSON_AddStringToObject(firmware, "url", "https://download.example.com/app.bin");
    cJSON_AddStringToObject(root, "future_optional_field", "ignored");
    char *json = cJSON_PrintUnformatted(root);
    CHECK(parse_response(json, "dev-001", &response));
    CHECK(strcmp(response.firmware.version, "2.0") == 0);
    free(json);
    cJSON_AddStringToObject(firmware, "version", "duplicate");
    json = cJSON_PrintUnformatted(root);
    CHECK(!parse_response(json, "dev-001", &response));
    free(json); cJSON_Delete(root);
    const char *reason;
    for (size_t i = 0; i < strlen(RESPONSE_JSON); ++i)
        CHECK(!device_handshake_parse_response(RESPONSE_JSON, i, "dev-001", &response, &reason));
    char malformed[1024];
    snprintf(malformed, sizeof(malformed), "%sx", RESPONSE_JSON);
    CHECK(!parse_response(malformed, "dev-001", &response));
    snprintf(malformed, sizeof(malformed), "%.*s,\"extra\":\"x\\u0000y\"}",
             (int)strlen(RESPONSE_JSON) - 1, RESPONSE_JSON);
    CHECK(!parse_response(malformed, "dev-001", &response));
    CHECK(strcmp(device_handshake_error_code("{\"code\":\"device_revoked\"}", 25), "device_revoked") == 0);
    const char *unknown = "{\"code\":\"sensitive server text\"}";
    CHECK(strcmp(device_handshake_error_code(unknown, strlen(unknown)), "http_error") == 0);
}

static void schedule_checks(void)
{
    device_handshake_schedule_t state = {0};
    CHECK(!device_handshake_due(&state, 0));
    device_handshake_network_changed(&state, true);
    CHECK(device_handshake_due(&state, 0));
    CHECK(device_handshake_complete(&state, state.generation, 503, false, 100));
    CHECK(!device_handshake_due(&state, 5099) && device_handshake_due(&state, 5100));
    device_handshake_network_changed(&state, false);
    device_handshake_network_changed(&state, true);
    CHECK(!device_handshake_due(&state, 5099));
    CHECK(device_handshake_complete(&state, state.generation, 0, false, 5100));
    CHECK(state.retry_at_ms == 20100);
    CHECK(device_handshake_complete(&state, state.generation, 429, false, 20100));
    CHECK(state.retry_at_ms == 80100);
    CHECK(device_handshake_complete(&state, state.generation, 500, false, 80100));
    CHECK(state.retry_at_ms == 140100);
    CHECK(device_handshake_complete(&state, state.generation, 200, true, 140100));
    CHECK(state.ready && !device_handshake_due(&state, 200000));
    uint32_t stale = state.generation;
    device_handshake_network_changed(&state, false);
    device_handshake_network_changed(&state, true);
    CHECK(!state.ready && !device_handshake_complete(&state, stale, 200, true, 200000));
    const int permanent[] = { 401, 403, 404, 301, 400, 200 };
    for (size_t i = 0; i < sizeof(permanent) / sizeof(permanent[0]); ++i) {
        device_handshake_refresh(&state);
        CHECK(device_handshake_complete(&state, state.generation, permanent[i], false, 200000));
        CHECK(state.blocked);
        device_handshake_network_changed(&state, false);
        device_handshake_network_changed(&state, true);
        CHECK(state.blocked && !device_handshake_due(&state, 99999999));
        device_handshake_refresh(&state);
        CHECK(device_handshake_due(&state, 200000));
    }
    CHECK(device_handshake_retryable(408) && device_handshake_retryable(599));
    CHECK(!device_handshake_retryable(600) && !device_handshake_retryable(302));
}

static void token_checks(void)
{
    const char *values[] = { "0", "-1", "1.5", "null", "true", "[]", "1e999",
                            "4294967296", "1", "300", "3600", "4294967295" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        cJSON *root = cJSON_Parse(RESPONSE_JSON);
        cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
        cJSON_DeleteItemFromObjectCaseSensitive(ws, "expires_in");
        cJSON_AddItemToObject(ws, "expires_in", cJSON_Parse(values[i]));
        char *json = cJSON_PrintUnformatted(root);
        device_handshake_response_t response;
        CHECK(parse_response(json, "dev-001", &response) == (i >= 8));
        free(json); cJSON_Delete(root);
    }
    CHECK(device_handshake_token_remaining_ms(3600, 1000, 1000) == 3600000);
    CHECK(device_handshake_token_remaining_ms(3600, 1000, 3301000) == 300000);
    CHECK(device_handshake_token_remaining_ms(3600, 1000, 3301001) == 299999);
    CHECK(device_handshake_token_remaining_ms(3600, 1000, 3601000) == 0);
    CHECK(device_handshake_token_remaining_ms(3600, 1000, 999) == 0);
    CHECK(device_handshake_token_remaining_ms(3600, -1, 0) == 0);
    CHECK(device_handshake_token_remaining_ms(0, 0, 0) == 0);
    CHECK(device_handshake_token_remaining_ms(UINT32_MAX, 0, 0) == 4294967295000LL);
    CHECK(device_handshake_token_remaining_ms(UINT32_MAX, 0, INT64_MAX) == 0);
    CHECK(device_handshake_token_remaining_ms(3600, INT64_MAX - 1000, INT64_MAX) == 3599000);
    device_handshake_schedule_t schedule = { .online = true, .ready = true, .generation = 9 };
    CHECK(device_handshake_renew(&schedule));
    CHECK(schedule.generation == 10 && !schedule.ready && device_handshake_due(&schedule, 0));
    CHECK(!device_handshake_renew(&schedule) && schedule.generation == 10);
    CHECK(device_handshake_complete(&schedule, 10, 503, false, 100));
    CHECK(!device_handshake_renew(&schedule) && !device_handshake_due(&schedule, 5099));
    CHECK(device_handshake_complete(&schedule, 10, 403, false, 5100));
    CHECK(!device_handshake_renew(&schedule) && schedule.blocked);
    device_handshake_network_changed(&schedule, false);
    device_handshake_network_changed(&schedule, true);
    CHECK(!device_handshake_renew(&schedule) && !device_handshake_due(&schedule, INT64_MAX));
}

typedef struct {
    const char *body;
    size_t length, offset, fragment;
    int status, fail_at_read, cancel_at_read;
    int64_t declared, clock_ms, read_delay_ms;
    unsigned opens, reads, closes;
    bool open_failed, incomplete;
} fake_http_t;

static bool fake_begin(void *ctx, const char *url, const device_handshake_header_t *headers,
                       size_t count, const char *body, int *status, int64_t *length)
{
    fake_http_t *fake = ctx;
    ++fake->opens;
    CHECK(strcmp(url, "https://example.com/base/api/device/v1/handshake") == 0);
    CHECK(strcmp(body, "{\"mac_address\":\"aa:bb:cc:dd:ee:01\"}") == 0);
    CHECK(count == 7);
    CHECK(strcmp(headers[0].name, "device-id") == 0 && strcmp(headers[0].value, "dev-001") == 0);
    CHECK(strcmp(headers[1].name, "device-model") == 0 && strcmp(headers[1].value, "esp32s3") == 0);
    CHECK(strcmp(headers[2].name, "device-version") == 0 && strcmp(headers[2].value, "1.4.2") == 0);
    CHECK(strcmp(headers[3].name, "authorization") == 0 && strcmp(headers[3].value, "Bearer test-only-psk") == 0);
    CHECK(strcmp(headers[4].value, "application/json") == 0);
    *status = fake->open_failed ? 0 : fake->status;
    *length = fake->declared;
    return !fake->open_failed;
}

static int fake_read(void *ctx, char *buffer, size_t size)
{
    fake_http_t *fake = ctx;
    ++fake->reads;
    fake->clock_ms += fake->read_delay_ms;
    if (fake->fail_at_read && (int)fake->reads >= fake->fail_at_read) return -1;
    size_t n = fake->length - fake->offset;
    if (n > size) n = size;
    if (n > fake->fragment) n = fake->fragment;
    memcpy(buffer, fake->body + fake->offset, n);
    fake->offset += n;
    return (int)n;
}
static bool fake_complete(void *ctx) { return !((fake_http_t *)ctx)->incomplete; }
static void fake_close(void *ctx) { ++((fake_http_t *)ctx)->closes; }
static bool fake_cancelled(void *ctx)
{
    fake_http_t *fake = ctx;
    return fake->cancel_at_read >= 0 && (int)fake->reads >= fake->cancel_at_read;
}
static int64_t fake_now(void *ctx) { return ((fake_http_t *)ctx)->clock_ms; }

static fake_http_t fake_default(void)
{
    return (fake_http_t){ .body = RESPONSE_JSON, .length = strlen(RESPONSE_JSON),
        .fragment = 7, .status = 200, .declared = (int64_t)strlen(RESPONSE_JSON), .cancel_at_read = -1 };
}

static void run_exchange(fake_http_t *fake, const char *version, device_handshake_result_t *result)
{
    device_handshake_config_t config;
    CHECK(parse_config(CONFIG_JSON, &config));
    const device_handshake_http_t http = { .context = fake, .begin = fake_begin, .read = fake_read,
        .complete = fake_complete, .close = fake_close, .cancelled = fake_cancelled, .now_ms = fake_now };
    device_handshake_exchange(&http, &config, version, "aa:bb:cc:dd:ee:01", result);
    CHECK(fake->closes == 1);
}

static void http_checks(void)
{
    device_handshake_result_t result;
    fake_http_t fake = fake_default();
    run_exchange(&fake, "1.4.2", &result);
    CHECK(result.success && result.status == 200 && fake.reads > 1);
    CHECK(strcmp(result.response.websocket.token, "test-only-token") == 0);
    fake = fake_default(); fake.declared = -1;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(result.success); /* chunked，未知总长度、多个短读。 */
    fake = fake_default(); fake.open_failed = true;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 0);
    fake = fake_default(); fake.cancel_at_read = 0;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && fake.opens == 0);
    fake = fake_default(); fake.cancel_at_read = 2;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 0 && fake.reads == 2);
    fake = fake_default(); fake.read_delay_ms = 15001;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 0 && fake.reads == 1);
    fake = fake_default(); fake.declared = DEVICE_HANDSHAKE_RESPONSE_MAX + 1;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 200 && fake.reads == 0);
    fake = fake_default(); ++fake.declared;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 0);
    fake = fake_default(); fake.incomplete = true;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 0);
    const int statuses[] = { 200, 401, 403, 404, 429, 503 };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        fake = fake_default(); fake.status = statuses[i]; fake.fail_at_read = 2;
        run_exchange(&fake, "1.4.2", &result);
        CHECK(!result.success && result.status == (statuses[i] == 200 ? 0 : statuses[i]));
    }
    fake = fake_default(); fake.status = 302;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 302 && fake.opens == 1 && fake.reads == 0);
    fake = fake_default(); fake.body = "{\"code\":\"device_credential_invalid\"}";
    fake.length = strlen(fake.body); fake.declared = (int64_t)fake.length; fake.status = 401;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 401 && strcmp(result.reason, "device_credential_invalid") == 0);
    fake = fake_default(); fake.body = "{}"; fake.length = 2; fake.declared = 2;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && result.status == 200); /* 完整但无效的 200 应停止自动重试。 */
    fake = fake_default(); run_exchange(&fake, "bad\r\nheader", &result);
    CHECK(!result.success && fake.opens == 0);
    char oversized[DEVICE_HANDSHAKE_RESPONSE_MAX + 1]; memset(oversized, ' ', sizeof(oversized));
    fake = fake_default(); fake.body = oversized; fake.length = sizeof(oversized); fake.declared = -1;
    fake.fragment = 512;
    run_exchange(&fake, "1.4.2", &result);
    CHECK(!result.success && strcmp(result.reason, "response_too_large") == 0);
}

int main(void)
{
    config_checks(); time_checks(); response_checks(); schedule_checks(); token_checks(); http_checks();
    printf("device_handshake: %u checks passed\n", checks);
    return 0;
}
