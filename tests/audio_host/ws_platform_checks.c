/* 直接编译 ws_service 的生产分发/快速中断函数；只替换播放器、RTOS 和网络。
 * 可注入 RX/worker 交错；不代替真实双核调度、TLS 或声学测试。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "../../main/event_bus.h"
#include "../../main/ws_service.h"
#include "../../main/response_stream.h"
#include "../../main/reply_boundary.h"
static unsigned checks, events[128], decoded, stopped, published, queued;
static uint32_t generation=7, speech_epoch, applied_speech_epoch, wire_turn, wire_done_turn;
static uint32_t played_frames, discarded_frames;
static uint32_t rx_audio_packets, rx_audio_last_ms, rx_audio_gap_max_ms, rx_audio_after_eof, queue_wait_max_ms;
static int state=WS_STATE_CONNECTED, last_control_event, interrupt_event;
static bool reminder_mode, rx_dispatching, stop_flag, drained;
static bool inject_speech_in_write;
static response_stream_t reply, wire_reply;
static SemaphoreHandle_t playback_lock=(void*)1;
static QueueHandle_t rx_queue=(void*)2;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void publish(app_event_id_t id,uint32_t gen)
{ CHECK(gen==generation); ++events[id]; ++published; }
#define current(p) ((p)->generation==generation && (state==WS_STATE_CONNECTED || state==WS_STATE_AUTHING))
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks) { (void)s; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return queued; }
static void log_lifecycle(const proto_msg_t *m,response_stream_action_t a) { (void)m; (void)a; }
static void log_server_error(const proto_msg_t *m) { CHECK(m->evt==PROTO_EVT_ERROR); }
static void audio_out_request_stop(void) { stop_flag=true; }
static bool audio_out_stop_requested(void) { return stop_flag; }
static esp_err_t audio_out_stop(void) { ++stopped; stop_flag=false; drained=true; return ESP_OK; }
static void audio_out_set_streaming(bool active) { if(active) drained=false; }
static bool audio_out_is_drained(void) { return drained; }
static bool observe_control(const proto_msg_t *msg);
static int audio_out_play_opus(const uint8_t *data,size_t len)
{
    CHECK(data!=NULL && len!=0);
    if(inject_speech_in_write) {
        inject_speech_in_write=false;
        proto_msg_t msg={.evt=PROTO_EVT_SPEECH_STARTED}; observe_control(&msg);
        return -1; /* 平台信号解除 PCM 背压，不得升级为会话错误。 */
    }
    ++decoded; return 320;
}
uint32_t esp_random(void) { return 1; }
int64_t esp_timer_get_time(void) { return 1000000; }
#include "ws_platform.inc"
static rx_frame_t packet(const char *text)
{
    proto_msg_t msg;
    rx_frame_t p={.data=(uint8_t*)text,.len=strlen(text),.opcode=1,.generation=generation};
    observe_rx_packet(&p);
    if(proto_parse_json(text,&msg)==PROTO_OK) CHECK(observe_control(&msg));
    p.turn=wire_turn; p.speech_epoch=speech_epoch;
    p.session_control=msg.evt==PROTO_EVT_ERROR || msg.evt==PROTO_EVT_PROXY_CONNECTED;
    return p;
}
static void control(const char *text) { rx_frame_t p=packet(text); handle_frame(&p); }
static rx_frame_t binary(void)
{
    rx_frame_t p={.data=(uint8_t*)"x",.len=1,.opcode=2,.generation=generation,
        .turn=wire_turn,.speech_epoch=speech_epoch};
    observe_rx_packet(&p);
    response_stream_accept_audio(&wire_reply,wire_turn);
    return p;
}
static void accept(void)
{
    ws_reply_offer_t offer; CHECK(ws_service_get_reply_offer(&offer));
    CHECK(ws_service_accept_reply(&offer));
}
static void reset(void)
{
    memset(events,0,sizeof(events)); published=decoded=stopped=queued=0;
    speech_epoch=applied_speech_epoch=wire_turn=wire_done_turn=0;
    rx_audio_packets=rx_audio_last_ms=rx_audio_gap_max_ms=rx_audio_after_eof=queue_wait_max_ms=0;
    response_stream_reset(&reply); response_stream_reset(&wire_reply);
    state=WS_STATE_CONNECTED; reminder_mode=rx_dispatching=stop_flag=false; drained=true;
}
int main(void)
{
    reset(); control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    control("{\"type\":\"response.created\",\"response_id\":\"old\"}");
    rx_frame_t old=binary(); handle_frame(&old); CHECK(decoded==1);
    rx_frame_t start=packet("{\"type\":\"input_audio_buffer.speech_started\"}");
    CHECK(stop_flag); handle_frame(&old); /* 旧队列未排完，平台信号已经生效。 */
    CHECK(stopped==1 && decoded==1 && ws_service_reply_interrupted());
    handle_frame(&start); CHECK(stopped==1 && events[EV_WS_RESPONSE_INTERRUPTED]==1);
    CHECK(events[EV_WS_ERROR]==0 && events[EV_WS_SERVER_ERROR]==0);
    control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    control("{\"type\":\"response.created\",\"response_id\":\"new\"}");
    proto_msg_t late; CHECK(proto_parse_json("{\"type\":\"response.cancelled\",\"response_id\":\"old\"}",&late)==PROTO_OK);
    CHECK(!observe_control(&late) && !stop_flag); /* 迟到旧取消不影响新回复。 */
    old=binary(); handle_frame(&old); CHECK(decoded==2);
    control("{\"type\":\"response.audio.done\",\"response_id\":\"new\"}");
    handle_frame(&old); CHECK(decoded==2); /* EOF 后不得偷偷重开。 */
    drained=true; finish_playback_if_drained(); CHECK(ws_service_reply_finished());
    CHECK(events[EV_WS_RESPONSE_DONE]==1);
    control("{\"type\":\"conversation.transcription\",\"who\":\"user\"}");
    CHECK(events[EV_WS_SERVER_ERROR]==0);

    reset(); control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    control("{\"type\":\"response.created\",\"response_id\":\"r\"}");
    old=binary(); handle_frame(&old);
    rx_frame_t cancel=packet("{\"type\":\"response.done\",\"response\":{\"id\":\"r\",\"status\":\"cancelled\"}}");
    handle_frame(&old); handle_frame(&cancel);
    CHECK(stopped==1 && decoded==1 && events[EV_WS_RESPONSE_INTERRUPTED]==1);
    CHECK(events[EV_WS_SERVER_ERROR]==0);
    control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    CHECK(proto_parse_json("{\"type\":\"response.cancelled\"}",&late)==PROTO_OK);
    CHECK(!observe_control(&late) && !stop_flag); /* 零等无活动 ack。 */
    old=binary(); inject_speech_in_write=true; handle_frame(&old);
    CHECK(events[EV_WS_SERVER_ERROR]==0 && stop_flag);
    apply_platform_interruption_locked(); CHECK(!stop_flag && ws_service_reply_interrupted());
    control("{\"type\":\"error\",\"code\":5000,\"message\":\"upstream failure\"}");
    CHECK(events[EV_WS_SERVER_ERROR]==1 && events[EV_WS_RESPONSE_DONE]==0);
    reset(); control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    rx_frame_t error=packet("{\"type\":\"error\",\"code\":5000,\"message\":\"network\"}");
    start=packet("{\"type\":\"input_audio_buffer.speech_started\"}");
    handle_frame(&error); CHECK(events[EV_WS_SERVER_ERROR]==1); /* 连接错误不得被音频代次过滤。 */
    handle_frame(&start);
    control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    control("{\"type\":\"response.created\",\"response_id\":\"failed\"}");
    control("{\"type\":\"response.done\",\"response\":{\"id\":\"failed\",\"status\":\"failed\"}}");
    CHECK(events[EV_WS_SERVER_ERROR]==1 && ws_service_reply_interrupted());

    /* worker 积压不会把 EOF 前到达的音频误记为 EOF 后到达。 */
    reset(); control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    old=binary(); CHECK(!old.after_audio_done && old.audio_sequence==1);
    rx_frame_t end=packet("{\"type\":\"response.audio.done\"}");
    rx_frame_t tail=binary();
    CHECK(end.audio_sequence==1 && tail.audio_sequence==2 && tail.after_audio_done);
    CHECK(rx_audio_after_eof==1);
    handle_frame(&old); CHECK(decoded==1);
    handle_frame(&end); handle_frame(&tail);
    CHECK(decoded==1 && discarded_frames==1 && events[EV_WS_SERVER_ERROR]==0);
    drained=true; finish_playback_if_drained(); CHECK(events[EV_WS_RESPONSE_DONE]==1);
    control("{\"type\":\"input_audio_buffer.speech_stopped\"}"); accept();
    tail=binary(); CHECK(!tail.after_audio_done); handle_frame(&tail); CHECK(decoded==2);
    printf("PASS: %u production WS platform interruption checks\n",checks); return 0;
}
