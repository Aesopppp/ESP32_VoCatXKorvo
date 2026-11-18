/* 直接运行提取的生产函数，仅替换 RTOS/transport/配置存储等环境依赖。
 * 不证明真实 TLS、任务并发或平台鉴权协议兼容；这些需编译与实板联调。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "../main/device_handshake_service.h"
#include "../main/device_handshake_http.h"
#include "../main/ws_service.h"
#include "../main/device_mqtt_service.h"
#include "cJSON.h"

#define ESP_ERR_INVALID_SIZE 5
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define CFG_KEY_WS_URL 1
#define CFG_KEY_ACOS_TOKEN 2
#define WIFI_STATE_GOT_IP 1
#define ACOS_WS_URL "wss://legacy.example/realtime"
#define DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS 300u
#define atomic_load(p) (*(p))
#define atomic_store(p, v) (*(p) = (v))
#define pdMS_TO_TICKS(ms) (ms)

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static const char *TAG = "test";
static int64_t test_now;
static int64_t now_ms(void) { return test_now; }
int64_t esp_timer_get_time(void) { return test_now * 1000; }
uint32_t esp_random(void) { return 1234; }

static bool s_initialized, s_disabled;
static device_handshake_config_t config;
static device_handshake_config_t *s_config;
static device_handshake_schedule_t s_schedule;
static unsigned s_seen_epoch, s_network_epoch, s_cancel_generation;
typedef struct { int64_t acquired_ms; device_handshake_result_t result; } test_snapshot_t;
static test_snapshot_t *s_snapshot;
static void clear_snapshot(void)
{
    if (s_snapshot) { memset(s_snapshot, 0, sizeof(*s_snapshot)); free(s_snapshot); }
    s_snapshot = NULL;
}
#include "device_handshake_service.c.inc"

static esp_websocket_client_handle_t client;
static ws_conn_state_t state;
static bool client_started, credentials_from_handshake, fail_alloc, fail_client, fail_send, fail_stop;
static proto_ws_credentials_t *credentials;
static unsigned creates, stops, connects, recoveries, ends;
static char sent[PROTO_REMINDER_AUTH_FRAME_SIZE], source_name[80], configured_url[PROTO_WS_URL_MAX + 1];
static size_t reminder_task_count;
static char reminder_device_id[65], reminder_task_ids[PROTO_REMINDER_TASK_IDS_MAX][129];
static void *heap_caps_calloc(size_t count, size_t size, int caps)
{ (void)caps; return fail_alloc ? NULL : calloc(count, size); }
static void *heap_caps_malloc(size_t size, int caps)
{ (void)caps; return fail_alloc ? NULL : malloc(size); }
static void cfg_service_get_str(int key, char *out, size_t size, const char *fallback)
{ (void)fallback; snprintf(out, size, "%s", key == CFG_KEY_WS_URL ? ACOS_WS_URL : "legacy-test-only"); }
static esp_websocket_client_handle_t create_client(const char *url)
{
    ++creates; snprintf(configured_url, sizeof(configured_url), "%s", url);
    return fail_client ? NULL : (esp_websocket_client_handle_t)(uintptr_t)(creates + 1);
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t handle)
{ CHECK(handle != NULL); return ESP_OK; }
static void log_auth_token_fingerprint(const char *token, const char *source)
{ CHECK(token && *token); snprintf(source_name, sizeof(source_name), "%s", source); }
int esp_websocket_client_send_text(esp_websocket_client_handle_t handle, const char *data, int len, int timeout)
{
    (void)timeout; CHECK(handle && len > 0 && (size_t)len < sizeof(sent));
    memcpy(sent, data, (size_t)len); sent[len] = 0;
    return fail_send ? -1 : len;
}
#include "ws_service.c.inc"

static bool session_requested, voice_auth_refresh_used, awaiting_voice_auth, waiting_for_credentials;
static device_mqtt_delivery_t *reminder_task;
esp_err_t ws_service_prepare_reminder(const char *device_id, const char *const *ids, size_t count)
{
    CHECK(count>0 && count<=PROTO_REMINDER_TASK_IDS_MAX && !client_started);
    strcpy(reminder_device_id,device_id); reminder_task_count=count;
    for(size_t i=0;i<count;++i) strcpy(reminder_task_ids[i],ids[i]);
    return ESP_OK;
}
static int64_t retry_at_ms, auth_deadline_ms;
static bool network_ready, provisioning;
static int wifi_service_get_state(void) { return network_ready ? WIFI_STATE_GOT_IP : 0; }
static bool provisioning_service_is_active(void) { return provisioning; }
esp_err_t ws_service_disconnect(void)
{
    ++stops;
    if (fail_stop) return ESP_FAIL;
    client_started = false; state = WS_STATE_DISCONNECTED; return ESP_OK;
}
esp_err_t ws_service_connect(void)
{ ++connects; CHECK(credentials && credentials->token[0]); client_started = true; return ESP_OK; }
static void end_conversation(const char *reason)
{ CHECK(reason != NULL); ++ends; session_requested = false; }
static void recover_session(const char *reason)
{
    CHECK(reason != NULL); ++recoveries;
    ws_service_disconnect(); retry_at_ms = now_ms() + 5000;
    awaiting_voice_auth = waiting_for_credentials = false;
}
#include "app_main.c.inc"

static void fresh(void)
{
    release_credentials(credentials); credentials = NULL;
    clear_snapshot();
    memset(&config, 0, sizeof(config)); config.enabled = true; s_config = &config;
    s_schedule = (device_handshake_schedule_t){ .online = true, .ready = true, .generation = 1 };
    s_initialized = true; s_disabled = false;
    s_seen_epoch = s_network_epoch = s_cancel_generation = 1;
    test_now = 1000;
    s_snapshot = calloc(1, sizeof(*s_snapshot)); CHECK(s_snapshot != NULL);
    s_snapshot->acquired_ms = 1000;
    strcpy(s_snapshot->result.response.websocket.url, "wss://gateway.example:8443/v1/realtime");
    strcpy(s_snapshot->result.response.websocket.token, "device-test-only");
    s_snapshot->result.response.websocket.expires_in = 3600;
    client = (esp_websocket_client_handle_t)(uintptr_t)1; state = WS_STATE_DISCONNECTED;
    client_started = credentials_from_handshake = fail_alloc = fail_client = fail_send = fail_stop = false;
    creates = stops = connects = recoveries = ends = 0;
    sent[0] = source_name[0] = configured_url[0] = 0;
    session_requested = network_ready = true; provisioning = false;
    voice_auth_refresh_used = awaiting_voice_auth = waiting_for_credentials = false;
    reminder_task=NULL; reminder_task_count=0; reminder_device_id[0]=0;
    retry_at_ms = test_now; auth_deadline_ms = 0;
}

static void builder_checks(void)
{
    char token[PROTO_AUTH_TOKEN_MAX + 2], buffer[PROTO_AUTH_FRAME_SIZE];
    const char *examples[] = { "device-token", "quote\"and\\slash", "line\n\t\x01", "UTF-8-测试" };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        size_t length = proto_build_auth_frame(buffer, sizeof(buffer), examples[i]);
        CHECK(length == strlen(buffer) && length > 0);
        cJSON *json = cJSON_Parse(buffer); CHECK(json != NULL);
        CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(json, "type")->valuestring, "auth") == 0);
        CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(json, "token")->valuestring, examples[i]) == 0);
        cJSON_Delete(json);
        CHECK(!proto_build_auth_frame(buffer, length, examples[i]) && !buffer[0]);
        CHECK(proto_build_auth_frame(buffer, length + 1, examples[i]) == length);
    }
    memset(token, '\x01', PROTO_AUTH_TOKEN_MAX); token[PROTO_AUTH_TOKEN_MAX] = 0;
    CHECK(proto_build_auth_frame(buffer, sizeof(buffer), token) > PROTO_AUTH_TOKEN_MAX * 6);
    token[PROTO_AUTH_TOKEN_MAX] = 'a'; token[PROTO_AUTH_TOKEN_MAX + 1] = 0;
    CHECK(!proto_build_auth_frame(buffer, sizeof(buffer), token));
    CHECK(!proto_build_auth_frame(buffer, sizeof(buffer), ""));
    CHECK(!proto_build_auth_frame(buffer, sizeof(buffer), NULL));
    CHECK(!proto_build_auth_frame(NULL, sizeof(buffer), "test"));
    char reminder_buffer[PROTO_REMINDER_AUTH_FRAME_SIZE];
    const char *task_ids[] = { "42", "43" };
    size_t reminder_length = proto_build_reminder_auth_frame(reminder_buffer,
        sizeof(reminder_buffer), "jwt-token", "dev-123", task_ids, 2);
    CHECK(reminder_length == strlen(reminder_buffer) && reminder_length > 0);
    cJSON *reminder = cJSON_Parse(reminder_buffer); CHECK(reminder != NULL);
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(reminder, "type")->valuestring, "auth"));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(reminder, "api_key")->valuestring, "jwt-token"));
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(reminder, "device_id")->valuestring, "dev-123"));
    cJSON *ids = cJSON_GetObjectItemCaseSensitive(reminder, "task_ids");
    CHECK(cJSON_GetArraySize(ids) == 2);
    CHECK(!strcmp(cJSON_GetArrayItem(ids, 0)->valuestring, "42"));
    CHECK(!strcmp(cJSON_GetArrayItem(ids, 1)->valuestring, "43"));
    cJSON_Delete(reminder);
    CHECK(!proto_build_reminder_auth_frame(reminder_buffer, sizeof(reminder_buffer),
        "jwt-token", "dev-123", (const char *const[]){"42", "x"}, 2));
    CHECK(!proto_build_reminder_auth_frame(reminder_buffer, sizeof(reminder_buffer),
        "jwt-token", "dev-123", task_ids, 0));
    proto_msg_t msg;
    CHECK(proto_parse_json("{\"type\":\"error\",\"code\":2000}", &msg) == PROTO_OK);
    CHECK(proto_is_auth_error(&msg));
    CHECK(proto_parse_json("{\"type\":\"error\",\"error\":{\"code\":\"token_expired\"}}", &msg) == PROTO_OK);
    CHECK(proto_is_auth_error(&msg));
    CHECK(proto_parse_json("{\"type\":\"error\",\"code\":500,\"message\":\"token_expired\"}", &msg) == PROTO_OK);
    CHECK(!proto_is_auth_error(&msg));
    msg.evt = PROTO_EVT_RESPONSE_DONE; msg.error_code = 2000;
    CHECK(!proto_is_auth_error(&msg) && !proto_is_auth_error(NULL));
}

static void connection_checks(void)
{
    fresh();
    device_mqtt_delivery_t batch={.device_id="dev-123",.count=2,.task_ids={"42","43"}};
    reminder_task=&batch; retry_voice_connection(test_now);
    CHECK(connects==1 && reminder_task_count==2);
    state=WS_STATE_AUTHING; CHECK(ws_service_authenticate()==ESP_OK);
    cJSON *reminder=cJSON_Parse(sent); CHECK(reminder!=NULL);
    CHECK(!strcmp(cJSON_GetObjectItem(reminder,"api_key")->valuestring,"device-test-only"));
    CHECK(!strcmp(cJSON_GetObjectItem(reminder,"device_id")->valuestring,"dev-123"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(reminder,"task_ids"))==2);
    CHECK(!cJSON_GetObjectItem(reminder,"content")); cJSON_Delete(reminder);
    fresh(); s_schedule.ready = false;
    for (unsigned i = 0; i < 10; ++i) retry_voice_connection(test_now);
    CHECK(waiting_for_credentials && !stops && !connects && !credentials);
    s_schedule.ready = true; retry_voice_connection(test_now);
    CHECK(connects == 1 && stops == 1 && !retry_at_ms && auth_deadline_ms == test_now + 20000);
    CHECK(credentials_from_handshake && strcmp(credentials->token, "device-test-only") == 0);
    CHECK(strcmp(configured_url, s_snapshot->result.response.websocket.url) == 0);
    clear_snapshot(); /* HTTP 刷新/清理不使当前 transport 的 token 指针悬空。 */
    state = WS_STATE_AUTHING;
    CHECK(ws_service_authenticate() == ESP_OK);
    CHECK(strcmp(source_name, "HTTP/handshake/websocket.token") == 0);
    cJSON *json = cJSON_Parse(sent); CHECK(json != NULL);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(json, "token")->valuestring, "device-test-only") == 0);
    cJSON_Delete(json);
    CHECK(ws_service_set_credentials("wss://other.example", "x") == ESP_ERR_INVALID_STATE);
    fresh(); s_disabled = true; retry_voice_connection(test_now);
    CHECK(connects == 1 && !credentials_from_handshake && strcmp(credentials->token, "legacy-test-only") == 0);
    state = WS_STATE_AUTHING; CHECK(ws_service_authenticate() == ESP_OK);
    CHECK(strcmp(source_name, "NVS/korvo_cfg/acos_token") == 0);
    fresh(); s_schedule.blocked = true; retry_voice_connection(test_now);
    CHECK(ends == 1 && !connects && !credentials);
    CHECK(!device_handshake_service_renew_credentials() && s_schedule.blocked);
    fresh(); s_initialized = false; retry_voice_connection(test_now);
    CHECK(ends == 1 && !connects);
    fresh(); s_config = NULL; retry_voice_connection(test_now); CHECK(ends == 1 && !connects);
    fresh(); ++s_network_epoch; retry_voice_connection(test_now);
    CHECK(!connects && !stops && waiting_for_credentials);
    fresh(); network_ready = false; retry_voice_connection(test_now); CHECK(!connects && !stops);
    network_ready = true; provisioning = true; retry_voice_connection(test_now); CHECK(!connects && !stops);
    fresh(); fail_alloc = true; retry_voice_connection(test_now);
    CHECK(!connects && retry_at_ms == test_now + 5000);
    fail_alloc = false; test_now += 5000; retry_voice_connection(test_now); CHECK(connects == 1);
    fresh(); fail_stop = true; retry_voice_connection(test_now); CHECK(!connects && !credentials);
}

static void lifetime_and_failure_checks(void)
{
    fresh(); test_now += 3300000;
    const device_handshake_response_t *response = NULL;
    CHECK(device_handshake_service_prepare_websocket(&response) == DEVICE_WS_READY && response);
    ++test_now;
    CHECK(device_handshake_service_prepare_websocket(&response) == DEVICE_WS_WAITING && !response);
    CHECK(!s_snapshot && !s_schedule.ready && s_schedule.generation == 2);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(device_handshake_service_prepare_websocket(&response) == DEVICE_WS_WAITING);
    CHECK(s_schedule.generation == 2); /* 轮询不会重新刷新或重置退避。 */
    fresh(); handle_voice_auth_failure("rejected");
    CHECK(voice_auth_refresh_used && recoveries == 1 && !ends && !s_snapshot);
    CHECK(s_schedule.generation == 2 && retry_at_ms == test_now + 5000);
    handle_voice_auth_failure("rejected again");
    CHECK(ends == 1 && recoveries == 1 && !session_requested && s_schedule.generation == 2);
    fresh(); s_schedule.blocked = true; handle_voice_auth_failure("rejected");
    CHECK(s_schedule.blocked && s_schedule.generation == 1);
    test_now += 5000; retry_voice_connection(test_now); CHECK(ends == 1 && !connects);
    fresh(); s_disabled = true; handle_voice_auth_failure("legacy invalid");
    CHECK(ends == 1 && !recoveries);
}

static void credential_boundary_checks(void)
{
    fresh(); char token[PROTO_AUTH_TOKEN_MAX + 2], url[PROTO_WS_URL_MAX + 2];
    memset(token, 't', PROTO_AUTH_TOKEN_MAX); token[PROTO_AUTH_TOKEN_MAX] = 0;
    memset(url, 'u', PROTO_WS_URL_MAX); memcpy(url, "wss://example.com/", 18); url[PROTO_WS_URL_MAX] = 0;
    CHECK(ws_service_set_credentials(url, token) == ESP_OK);
    token[0] = 'z'; url[18] = 'z';
    CHECK(credentials->token[0] == 't' && credentials->url[18] == 'u');
    state = WS_STATE_AUTHING; CHECK(ws_service_authenticate() == ESP_OK);
    cJSON *json = cJSON_Parse(sent); CHECK(json != NULL);
    CHECK(strlen(cJSON_GetObjectItemCaseSensitive(json, "token")->valuestring) == PROTO_AUTH_TOKEN_MAX);
    cJSON_Delete(json);
    fail_send = true; CHECK(ws_service_authenticate() == ESP_FAIL);
    fail_send = false; fail_alloc = true; CHECK(ws_service_authenticate() == ESP_ERR_NO_MEM);
    fail_alloc = false; state = WS_STATE_DISCONNECTED;
    CHECK(ws_service_set_credentials(NULL, token) == ESP_ERR_INVALID_ARG);
    token[PROTO_AUTH_TOKEN_MAX] = 'x'; token[PROTO_AUTH_TOKEN_MAX + 1] = 0;
    CHECK(ws_service_set_credentials(url, token) == ESP_ERR_INVALID_SIZE);
    token[PROTO_AUTH_TOKEN_MAX] = 0; url[PROTO_WS_URL_MAX] = 'x'; url[PROTO_WS_URL_MAX + 1] = 0;
    CHECK(ws_service_set_credentials(url, token) == ESP_ERR_INVALID_SIZE);
    fail_client = true;
    CHECK(ws_service_set_credentials("wss://new.example", "new-token") == ESP_ERR_NO_MEM);
    CHECK(credentials->token[0] == 't'); /* 配置失败不留下半组 URL/token。 */
    fail_client = false;
    CHECK(ws_service_set_credentials("wss://new.example", "new-token") == ESP_OK);
    CHECK(strcmp(credentials->url, "wss://new.example") == 0 && strcmp(credentials->token, "new-token") == 0);
}

int main(void)
{
    builder_checks(); connection_checks(); lifetime_and_failure_checks(); credential_boundary_checks();
    release_credentials(credentials); clear_snapshot();
    printf("PASS: %u device voice authentication checks (production code; mocked transport)\n", checks);
    return 0;
}
