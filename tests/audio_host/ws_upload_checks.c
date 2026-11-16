/* 直接运行生产发送函数和 JSON builder；Base64/网络使用边界可注入替身。
 * 检查固定缓冲复用后的消息、长度和错误恢复，不替代 TLS/多核测试。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "../../main/ws_service.h"
#include "../../main/proto_service.h"
#include "cJSON.h"
static unsigned checks, sent;
static int state = WS_STATE_CONNECTED;
static bool reminder_mode, fail_base64, fail_send, reenter;
static const uint8_t *expected_data;
static size_t expected_len;
static char expected_audio[345];
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
uint32_t esp_random(void) { return 1; }
int64_t esp_timer_get_time(void) { return 1000000; }
static int mbedtls_base64_encode(unsigned char *dst, size_t cap, size_t *written,
                                 const unsigned char *src, size_t len)
{
    CHECK(src == expected_data && len == expected_len);
    CHECK(cap >= strlen(expected_audio) + 1);
    if (fail_base64) return -1;
    *written = strlen(expected_audio); memcpy(dst, expected_audio, *written + 1);
    return 0;
}
esp_err_t ws_service_send_json(const char *text)
{
    cJSON *root = cJSON_Parse(text); CHECK(root);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")), "input_audio_buffer.append"));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "encoding")), "opus"));
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "audio")), expected_audio));
    CHECK(cJSON_IsString(cJSON_GetObjectItem(root, "event_id")));
    cJSON_Delete(root); ++sent;
    if (reenter) CHECK(ws_service_send_opus_frame(expected_data, expected_len) == ESP_ERR_INVALID_STATE);
    return fail_send ? ESP_FAIL : ESP_OK;
}
#include "ws_upload.inc"
int main(void)
{
    uint8_t data[WS_OPUS_FRAME_MAX_BYTES + 1] = {0};
    expected_data = data; expected_len = WS_OPUS_FRAME_MAX_BYTES;
    memset(expected_audio, 'A', 342); strcpy(expected_audio + 342, "==");
    CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_OK);
    /* 从最大包切到小包，JSON 不得残留上一包尾部。 */
    data[0] = 255; expected_len = 1; strcpy(expected_audio, "/w==");
    CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_OK);
    data[1] = 239; expected_len = 2; strcpy(expected_audio, "/+8=");
    CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_OK);
    data[0] = 1; data[1] = 2; data[2] = 3; expected_len = 3; strcpy(expected_audio, "AQID");
    reenter = true; CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_OK); reenter = false;
    CHECK(sent == 4);
    CHECK(ws_service_send_opus_frame(data, 0) == ESP_ERR_INVALID_ARG);
    CHECK(ws_service_send_opus_frame(NULL, 1) == ESP_ERR_INVALID_ARG);
    CHECK(ws_service_send_opus_frame(data, sizeof(data)) == ESP_ERR_INVALID_ARG);
    state = WS_STATE_DISCONNECTED;
    CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_ERR_INVALID_STATE);
    state = WS_STATE_CONNECTED; reminder_mode = true;
    CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_ERR_INVALID_STATE); reminder_mode = false;
    fail_base64 = true; CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_FAIL);
    fail_base64 = false; fail_send = true; CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_FAIL);
    fail_send = false; CHECK(ws_service_send_opus_frame(data, expected_len) == ESP_OK);
    CHECK(sent == 6); /* 失败不重发；所有出口都释放缓冲的独占标记。 */
    printf("PASS: %u production upload JSON checks (bounded buffers, reuse, invalid state, failure, concurrent misuse)\n", checks);
    return 0;
}
