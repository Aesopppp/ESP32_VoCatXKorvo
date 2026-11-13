/* 运行生产 JSON 解析和回复流策略，重放工具调用、多段音频与取消的事件顺序。
 * 主机命令见 docs/acos-response-lifecycle.md；不模拟 DMA/RTOS/平台服务。 */
#include "../main/response_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

/* 仅给未参与网络的 builder 提供确定性依赖。 */
uint32_t esp_random(void) { return 0x123456u; }
int64_t esp_timer_get_time(void) { return 1234000; }

static proto_msg_t parse(const char *json)
{
    proto_msg_t msg;
    CHECK(proto_parse_json(json, &msg) == PROTO_OK);
    return msg;
}

static response_stream_action_t control(response_stream_t *stream, const char *json)
{
    proto_msg_t msg = parse(json);
    return response_stream_control(stream, stream->turn, &msg);
}

static void parser_checks(void)
{
    proto_msg_t msg = parse("{\"type\":\"response.audio.done\",\"response_id\":\"resp_1\","
                            "\"item_id\":\"item_1\",\"output_index\":0,\"content_index\":0}");
    CHECK(msg.evt == PROTO_EVT_RESPONSE_DONE && strcmp(msg.response_id, "resp_1") == 0);
    msg = parse("{\"response\":{\"output\":[{\"type\":\"function_call\",\"status\":\"in_progress\"}],"
                "\"id\":\"resp_2\",\"status\":\"completed\"},\"type\":\"response.done\"}");
    CHECK(msg.evt == PROTO_EVT_RESPONSE_TERMINAL && msg.has_tool_call);
    CHECK(strcmp(msg.status, "completed") == 0 && strcmp(msg.response_id, "resp_2") == 0);
    msg = parse("{\"error\":{\"type\":\"invalid_request_error\",\"code\":1001,"
                "\"message\":\"test error\"},\"type\":\"error\"}");
    CHECK(msg.evt == PROTO_EVT_ERROR && msg.error_code == 1001 && strcmp(msg.error_msg, "test error") == 0);
    msg = parse("{\"type\":\"error\",\"error\":{\"code\":\"response_cancel_not_active\",\"message\":\"No active response\"}}");
    CHECK(strcmp(msg.error_code_text, "response_cancel_not_active") == 0);
    msg = parse("{\"type\":\"error\",\"code\":\"legacy_error\",\"message\":\"legacy\"}");
    CHECK(strcmp(msg.error_msg, "legacy") == 0);
    char json[256] = "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_owned\"}}";
    CHECK(proto_parse_json(json, &msg) == PROTO_OK);
    memset(json, 'x', strlen(json));
    CHECK(strcmp(msg.response_id, "resp_owned") == 0); /* 无悬空 JSON 借用指针 */
    msg = parse("{\"type\":\"response.done\",\"response\":{\"status\":\"incomplete\","
                "\"status_details\":{\"reason\":\"max_output_tokens\"}}}");
    CHECK(strcmp(msg.reason, "max_output_tokens") == 0);
    const char *invalid[] = {
        "{\"type\":\"response.audio.done\"", "{\"type\":\"response.audio.done\"} trailing",
        "{\"type\":\"response.audio.done\",\"type\":\"error\"}",
        "{\"type\":\"response.created\",\"response_id\":\"a\",\"response\":{\"id\":\"b\"}}",
        "{\"type\":\"response.created\"}", "{\"type\":7}",
        "{\"type\":\"response.audio.done\",\"response_id\":\"a\\u0000b\"}",
        "{\"type\":\"response.done\",\"response\":[]}", "[]",
        "{\"data\":{\"type\":\"response.audio.done\"}}",
        "{\"type\":\"error\",\"error\":{\"code\":\"a\",\"code\":\"b\"}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(proto_parse_json(invalid[i], &msg) != PROTO_OK);
    char long_id[PROTO_RESPONSE_ID_SIZE + 1];
    memset(long_id, 'a', sizeof(long_id) - 1); long_id[sizeof(long_id) - 1] = 0;
    char oversized[512];
    snprintf(oversized, sizeof(oversized), "{\"type\":\"response.created\",\"response_id\":\"%s\"}", long_id);
    CHECK(proto_parse_json(oversized, &msg) == PROTO_ERR_PARSE_FIELD);
    char nested[128];
    memset(nested, '[', 16); memset(nested + 16, ']', 16); nested[32] = 0;
    CHECK(proto_parse_json(nested, &msg) == PROTO_ERR_INVALID_JSON);
    CHECK(proto_parse_json("{\"type\":\"future.event\"}", &msg) == PROTO_ERR_UNKNOWN_TYPE);
}

static void normal_reply_checks(void)
{
    response_stream_t stream;
    response_stream_begin(&stream, 1);
    CHECK(control(&stream, "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}") == RESPONSE_STREAM_NONE);
    CHECK(stream.phase == RESPONSE_WAITING && !stream.audio_eof);
    CHECK(!response_stream_finish_if_drained(&stream, true, true, true));
    CHECK(response_stream_accept_audio(&stream, 1));
    CHECK(control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"reply_1\"}") == RESPONSE_STREAM_AUDIO_END);
    CHECK(!response_stream_finish_if_drained(&stream, false, true, true)); /* PCM 尾音 */
    CHECK(response_stream_finish_if_drained(&stream, true, true, true));
    CHECK(!response_stream_finish_if_drained(&stream, true, true, true)); /* 只完成一次 */
    CHECK(!response_stream_accept_audio(&stream, 1)); /* 迟到 raw 音频不重新开流 */
    CHECK(control(&stream, "{\"type\":\"response.audio.done\"}") == RESPONSE_STREAM_NONE);

    response_stream_begin(&stream, 4);
    for (unsigned i=0;i<76;++i) CHECK(response_stream_accept_audio(&stream,4));
    CHECK(control(&stream,"{\"type\":\"response.audio.done\"}")==RESPONSE_STREAM_AUDIO_END);
    for (unsigned i=0;i<100;++i) CHECK(!response_stream_accept_audio(&stream,4));
    CHECK(!response_stream_finish_if_drained(&stream,false,true,true));
    CHECK(response_stream_finish_if_drained(&stream,true,true,true));
    CHECK(!response_stream_accept_audio(&stream,3));

    response_stream_begin(&stream, 5);
    control(&stream, "{\"type\":\"response.output_item.added\",\"item\":{\"type\":\"message\"}}");
    CHECK(response_stream_accept_audio(&stream, 5));
    control(&stream, "{\"type\":\"response.audio.done\"}");
    control(&stream, "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\"}}");
    CHECK(stream.audio_eof); /* 消息项结束不能撤销音频结束 */
    CHECK(response_stream_finish_if_drained(&stream, true, true, true));
}

static void tool_reply_checks(void)
{
    response_stream_t stream;
    response_stream_begin(&stream, 8);
    control(&stream, "{\"type\":\"response.created\",\"response\":{\"id\":\"tool_1\"}}");
    control(&stream, "{\"type\":\"response.output_item.added\",\"response_id\":\"tool_1\",\"item\":{\"type\":\"function_call\"}}");
    control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"tool_1\"}");
    CHECK(!stream.tool_pending && stream.audio_eof);
    control(&stream, "{\"type\":\"response.done\",\"response\":{\"id\":\"tool_1\",\"status\":\"completed\","
                     "\"output\":[{\"type\":\"function_call\",\"name\":\"create_reminder\"}]}}");
    CHECK(!stream.tool_pending && stream.audio_eof);
    control(&stream, "{\"type\":\"response.created\",\"response\":{\"id\":\"reply_2\"}}");
    CHECK(!stream.tool_pending && stream.phase == RESPONSE_WAITING);
    CHECK(response_stream_accept_audio(&stream, 8));
    CHECK(control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"tool_1\"}") == RESPONSE_STREAM_NONE);
    CHECK(!stream.audio_eof);
    CHECK(control(&stream, "{\"type\":\"response.done\",\"response\":{\"id\":\"reply_2\",\"status\":\"completed\"}}") == RESPONSE_STREAM_NONE);
    CHECK(!stream.audio_eof);
    control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"reply_2\"}");
    CHECK(response_stream_finish_if_drained(&stream, true, true, true));
    CHECK(control(&stream, "{\"type\":\"response.created\",\"response\":{\"id\":\"reply_2\"}}") == RESPONSE_STREAM_NONE);
    CHECK(stream.phase == RESPONSE_IDLE); /* 重复 created 不重开 */
    CHECK(control(&stream, "{\"type\":\"response.created\",\"response\":{\"id\":\"reply_3\"}}") == RESPONSE_STREAM_CONTINUED);
    CHECK(response_stream_accept_audio(&stream, 8));
    control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"reply_3\"}");
    CHECK(response_stream_finish_if_drained(&stream, true, true, true));
    response_stream_user_started(&stream);
    control(&stream, "{\"type\":\"response.created\",\"response\":{\"id\":\"late_4\"}}");
    CHECK(stream.phase == RESPONSE_DISCARD && !response_stream_accept_audio(&stream, 8));
}

static void tool_audio_fallback_checks(void)
{
    for (unsigned with_created=0;with_created<2;++with_created) {
        response_stream_t stream;
        response_stream_begin(&stream,9);
        if(with_created) control(&stream,"{\"type\":\"response.created\",\"response_id\":\"tool_1\"}");
        control(&stream,"{\"type\":\"response.done\",\"response\":{\"id\":\"tool_1\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\"}]}}");
        CHECK(stream.tool_pending);
        CHECK(!response_stream_accept_audio(&stream,8));
        CHECK(response_stream_accept_audio(&stream,9));
        CHECK(!stream.tool_pending);
        control(&stream,"{\"type\":\"response.audio.done\"}");
        CHECK(!response_stream_accept_audio(&stream,9));
        control(&stream,"{\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\"}}");
        CHECK(stream.audio_eof && !stream.tool_pending);
        CHECK(!response_stream_finish_if_drained(&stream,false,true,true));
        CHECK(response_stream_finish_if_drained(&stream,true,true,true));
        CHECK(control(&stream,"{\"type\":\"response.created\",\"response_id\":\"tool_2\"}")==RESPONSE_STREAM_CONTINUED);
        CHECK(response_stream_accept_audio(&stream,9));
        response_stream_user_started(&stream);
        CHECK(!response_stream_accept_audio(&stream,9));
        CHECK(control(&stream,"{\"type\":\"response.audio.done\"}")==RESPONSE_STREAM_NONE);
    }
}


static void failure_checks(void)
{
    const char *events[] = {
        "{\"type\":\"response.done\",\"response\":{\"status\":\"failed\"}}",
        "{\"type\":\"response.done\",\"status\":\"incomplete\"}"
    };
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        response_stream_t stream;
        response_stream_begin(&stream, 1);
        CHECK(control(&stream, events[i]) == RESPONSE_STREAM_FAILED);
        CHECK(!response_stream_accept_audio(&stream, 1));
        CHECK(!response_stream_finish_if_drained(&stream, true, true, true));
    }
}


static void platform_interruption_checks(void)
{
    response_stream_t stream;
    response_stream_begin(&stream, 10);
    control(&stream, "{\"type\":\"response.created\",\"response_id\":\"old\"}");
    CHECK(response_stream_accept_audio(&stream, 10));
    response_stream_user_started(&stream);
    CHECK(stream.phase == RESPONSE_DISCARD && !stream.input_held);
    CHECK(!response_stream_accept_audio(&stream, 10));
    CHECK(control(&stream, "{\"type\":\"response.done\",\"response\":{\"id\":\"old\",\"status\":\"cancelled\"}}") == RESPONSE_STREAM_NONE);
    CHECK(control(&stream, "{\"type\":\"response.created\",\"response_id\":\"old\"}") == RESPONSE_STREAM_NONE);
    /* 不需要 cancel ack；平台新断句直接开启下一轮。 */
    response_stream_next_turn(&stream, 11);
    response_stream_hold_input(&stream);
    CHECK(!response_stream_release_input(&stream, 10));
    CHECK(!response_stream_accept_audio(&stream, 10));
    CHECK(control(&stream, "{\"type\":\"response.canceled\",\"response_id\":\"old\"}") == RESPONSE_STREAM_NONE);
    CHECK(control(&stream, "{\"type\":\"response.audio.done\",\"response_id\":\"old\"}") == RESPONSE_STREAM_NONE);
    CHECK(stream.phase == RESPONSE_WAITING && !stream.audio_eof);
    CHECK(response_stream_release_input(&stream, 11));
    control(&stream, "{\"type\":\"response.created\",\"response_id\":\"new\"}");
    CHECK(response_stream_accept_audio(&stream, 11));
    CHECK(control(&stream, "{\"type\":\"response.cancelled\",\"response_id\":\"old\"}") == RESPONSE_STREAM_NONE);
    CHECK(control(&stream, "{\"type\":\"response.done\",\"response\":{\"id\":\"new\",\"status\":\"cancelled\"}}") == RESPONSE_STREAM_CANCELLED);
    CHECK(!response_stream_accept_audio(&stream, 11));
    response_stream_next_turn(&stream, 12);
    CHECK(control(&stream, "{\"type\":\"response.canceled\"}") == RESPONSE_STREAM_NONE);
    CHECK(stream.phase == RESPONSE_WAITING); /* 无 ID 的旧取消不影响尚未开始的新回答 */
    proto_msg_t msg = parse("{\"type\":\"conversation.item.input_audio_transcription.started\"}");
    CHECK(msg.evt == PROTO_EVT_SPEECH_STARTED);
    msg = parse("{\"type\":\"conversation.item.input_audio_transcription.completed\"}");
    CHECK(response_stream_starts_turn(&msg, false));
    CHECK(!response_stream_starts_turn(&msg, true));
    msg = parse("{\"type\":\"response.output_audio.done\"}");
    CHECK(msg.evt == PROTO_EVT_RESPONSE_DONE);
}

int main(void)
{
    parser_checks();
    normal_reply_checks();
    tool_reply_checks();
    tool_audio_fallback_checks();
    platform_interruption_checks();
    failure_checks();

    printf("PASS: %u response stream checks (production JSON parser and lifecycle)\n", checks);
    return 0;
}
