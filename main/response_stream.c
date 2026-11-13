#include "response_stream.h"
#include "reply_boundary.h"
#include <string.h>

static bool active(const response_stream_t *stream)
{
    return stream->phase == RESPONSE_WAITING || stream->phase == RESPONSE_PLAYING;
}

static bool cancelled(const proto_msg_t *msg)
{
    return strcmp(msg->status, "cancelled") == 0 || strcmp(msg->status, "canceled") == 0 ||
           strcmp(msg->reason, "cancelled") == 0 || strcmp(msg->reason, "canceled") == 0;
}

bool response_stream_starts_turn(const proto_msg_t *msg, bool reminder)
{
    return msg->evt == PROTO_EVT_SPEECH_STOPPED && !reminder;
}

void response_stream_reset(response_stream_t *stream)
{
    memset(stream, 0, sizeof(*stream));
    stream->phase = RESPONSE_IDLE;
}

void response_stream_begin(response_stream_t *stream, uint32_t turn)
{
    response_stream_reset(stream);
    stream->turn = turn;
    stream->continuation_allowed = true;
    response_expect(&stream->phase);
}

/* 平台确认用户开始说话后，立即废弃旧输出；没有取消请求/确认等待阶段。 */
void response_stream_user_started(response_stream_t *stream)
{
    stream->phase = RESPONSE_DISCARD;
    stream->continuation_allowed = false;
    stream->input_held = stream->tool_pending = stream->audio_eof = false;
}

/* 新用户回合保留最近退役 ID，防止旧终止事件落到尚未收到 created 的新回复。 */
void response_stream_next_turn(response_stream_t *stream, uint32_t turn)
{
    char retired[PROTO_RESPONSE_ID_SIZE];
    strcpy(retired, stream->response_id[0] ? stream->response_id : stream->retired_id);
    response_stream_begin(stream, turn);
    strcpy(stream->retired_id, retired);
}

bool response_stream_accept_audio(response_stream_t *stream, uint32_t turn)
{
    if (stream->input_held || stream->audio_eof || !reply_packet_current(stream->turn, turn) ||
        !response_accept_audio(&stream->phase))
        return false;
    /* 每轮只有一个结束边界。audio.done 后只排空已缓存 PCM，迟到二进制
     * 不能重新开窗；下一次断句或明确的新 response.created 才能开启新输出。 */
    stream->tool_pending = false;
    stream->audio_eof = false;
    return true;
}

void response_stream_hold_input(response_stream_t *stream)
{
    stream->input_held = true;
}

bool response_stream_input_held(const response_stream_t *stream, uint32_t turn)
{
    return stream->input_held && stream->turn == turn && active(stream);
}

bool response_stream_release_input(response_stream_t *stream, uint32_t turn)
{
    if (!response_stream_input_held(stream, turn)) return false;
    stream->input_held = false;
    return true;
}

static bool matches_id(const response_stream_t *stream, const proto_msg_t *msg)
{
    /* V1 的 created/ID 都不是必填；无 created 时不能给无标识二进制强加 ID。
     * 一旦收到明确的 created，则有 ID 的旧结束事件必须与当前阶段严格匹配。 */
    return !stream->id_from_created || !msg->response_id[0] ||
           strcmp(stream->response_id, msg->response_id) == 0;
}

response_stream_action_t response_stream_control(response_stream_t *stream, uint32_t turn,
                                                  const proto_msg_t *msg)
{
    if (!reply_packet_current(stream->turn, turn)) return RESPONSE_STREAM_NONE;
    if (msg->evt == PROTO_EVT_RESPONSE_CREATED) {
        if (!msg->response_id[0] ||
            stream->phase == RESPONSE_DISCARD ||
            (!active(stream) && !stream->continuation_allowed) ||
            strcmp(stream->response_id, msg->response_id) == 0 ||
            strcmp(stream->retired_id, msg->response_id) == 0) return RESPONSE_STREAM_NONE;
        bool continued = stream->phase == RESPONSE_IDLE;
        if (stream->response_id[0]) strcpy(stream->retired_id, stream->response_id);
        strcpy(stream->response_id, msg->response_id);
        stream->id_from_created = true;
        stream->tool_pending = stream->audio_eof = false;
        response_expect(&stream->phase);
        return continued ? RESPONSE_STREAM_CONTINUED : RESPONSE_STREAM_NONE;
    }
    if ((msg->response_id[0] && strcmp(stream->retired_id, msg->response_id) == 0) ||
        !matches_id(stream, msg)) return RESPONSE_STREAM_NONE;
    if (!active(stream)) return RESPONSE_STREAM_NONE;
    if (msg->evt == PROTO_EVT_RESPONSE_TERMINAL && cancelled(msg)) {
        /* 无 ID、无音频的新候选不能被旧 cancelled 消息关闭。平台可直接发
         * speech_started 中断；有当前 response ID 的主动取消也正常收尾。 */
        if (stream->phase != RESPONSE_PLAYING && !stream->id_from_created)
            return RESPONSE_STREAM_NONE;
        response_stream_user_started(stream);
        return RESPONSE_STREAM_CANCELLED;
    }
    /* 正常结束是关闭窗口，之后的工具/消息事件不能重新接纳同轮迟到音频。
     * 仅前面的新 created 分支或下一次 speech_stopped 能重新开窗。 */
    if (stream->audio_eof) return RESPONSE_STREAM_NONE;
    if (msg->evt == PROTO_EVT_RESPONSE_TOOL ||
        (msg->evt == PROTO_EVT_RESPONSE_TERMINAL && msg->has_tool_call)) {
        stream->tool_pending = true;
        stream->audio_eof = false;
    } else if (msg->evt == PROTO_EVT_RESPONSE_MESSAGE) {
        stream->tool_pending = false;
        /* output_item.done 常在 audio.done 之后到达，只关闭消息项，
         * 不能撤销已经收到的音频 EOF；added 才表示下一段即将开始。 */
        if (strcmp(msg->raw_type, "response.output_item.added") == 0)
            stream->audio_eof = false;
    } else if (msg->evt == PROTO_EVT_RESPONSE_DONE) {
        if (!stream->id_from_created && msg->response_id[0])
            strcpy(stream->response_id, msg->response_id);
        stream->audio_eof = true;
        stream->tool_pending = false;
        return RESPONSE_STREAM_AUDIO_END;
    }
    /* failed/incomplete 结束本轮并回到监听，不自行断开正常连接。
     * completed 只是阶段完成，正常音频仍以 audio.done 为结束边界。 */
    if (msg->evt == PROTO_EVT_RESPONSE_TERMINAL &&
        (strcmp(msg->status, "failed") == 0 || strcmp(msg->status, "incomplete") == 0)) {
        stream->phase = RESPONSE_DISCARD;
        stream->continuation_allowed = false;
        return RESPONSE_STREAM_FAILED;
    }
    return RESPONSE_STREAM_NONE;
}

bool response_stream_finish_if_drained(response_stream_t *stream, bool drained,
                                       bool queue_empty, bool wire_audio_done)
{
    /* worker 不能在第一个 audio.done 之后越过 FIFO 中仍待处理的后续段。
     * wire_audio_done 防止 RX 已收到下一段、但尚未完成入队时抢先完成。 */
    if (stream->input_held || stream->tool_pending || !queue_empty || !wire_audio_done ||
        !response_playback_finished(stream->phase, stream->audio_eof, drained)) return false;
    response_finish(&stream->phase);
    stream->audio_eof = false;
    return true;
}
