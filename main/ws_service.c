#include "ws_service.h"
#include "audio_out.h"
#include "proto_service.h"
#include "response_stream.h"
#include "ws_rx_policy.h"
#include "reply_boundary.h"
#include "reminder_playback_policy.h"
#include "event_bus.h"
#include "cfg_service.h"
#include "board_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ws_service";
static esp_websocket_client_handle_t client;
static atomic_int state = WS_STATE_DISCONNECTED;
static atomic_uint generation = 1;
static bool client_started;
static proto_ws_credentials_t *credentials; /* 主任务设置；RX 仅在 playback_lock 下读取。 */
static bool credentials_from_handshake;
static uint8_t *frame;
static size_t frame_size, received;
static int opcode;
static QueueHandle_t rx_queue;
static SemaphoreHandle_t playback_lock;
static TaskHandle_t rx_task;
static response_stream_t reply; /* playback_lock 保护，包含音频段/工具阶段状态 */
static uint32_t rx_drop_count;
static atomic_size_t rx_bytes;
static atomic_uint wire_turn, wire_done_turn;
static uint32_t played_frames, discarded_frames;
/* 回调仅推进打断代次；worker 在解码下一包之前清理播放器。
 * 不把平台打断排在数秒旧音频后，也不在网络回调中执行 I2S/阻塞操作。 */
static atomic_uint speech_epoch;
static uint32_t applied_speech_epoch; /* playback_lock 保护 */
/* 只由网络回调推进的协议镜像，无解码/锁等待。用于在排队音频之前校验
 * 平台取消的 response ID；主任务仅在 transport 停止后复位它。 */
static response_stream_t wire_reply;
static atomic_int interrupt_event;
static atomic_bool rx_dispatching;
static uint32_t lifecycle_log_ms; /* worker 专用，限制扩展事件诊断频率 */
static atomic_int last_control_event;
static atomic_uint last_rx_ms;
/* 回调统计只记数量/时刻，不记录音频、正文或凭据。累积到本连接断开。 */
static atomic_uint rx_audio_packets, rx_audio_last_ms, rx_audio_gap_max_ms, rx_audio_after_eof;
static uint32_t queue_wait_max_ms; /* worker 专用，包含正常 PCM 背压等待。 */
static atomic_bool reminder_mode;
static uint32_t reminder_started_ms; /* playback_lock 保护 */
static atomic_uint reminder_rx_ms;
typedef struct {
    uint8_t *data;
    size_t len;
    int opcode;
    uint32_t generation;
    uint32_t turn;
    uint32_t speech_epoch;
    bool session_control; /* 鉴权/error 属于连接，不因回答被打断而丢弃。 */
    uint32_t received_ms, audio_sequence;
    bool after_audio_done; /* 入队时已在最终 audio.done 之后，不能由 worker 延迟推断。 */
} rx_frame_t;
static void apply_platform_interruption_locked(void);

/* 完整 WS 消息重组后调用；与 control 共用同一回调，保留真实接收顺序。 */
static void observe_rx_packet(rx_frame_t *packet)
{
    packet->received_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (packet->opcode == 2) {
        uint32_t previous = atomic_exchange(&rx_audio_last_ms, packet->received_ms);
        uint32_t sequence = atomic_fetch_add(&rx_audio_packets, 1);
        uint32_t gap = packet->received_ms - previous;
        /* 人声输入/等候回答不算流内供音间隔；开始新窗口时清 last_ms。 */
        if (sequence && previous && gap > atomic_load(&rx_audio_gap_max_ms))
            atomic_store(&rx_audio_gap_max_ms, gap);
        packet->after_audio_done = atomic_load(&reminder_mode) ?
            reply_boundary_received(atomic_load(&wire_turn), atomic_load(&wire_done_turn)) :
            wire_reply.audio_eof;
        if (packet->after_audio_done) atomic_fetch_add(&rx_audio_after_eof, 1);
    }
    packet->audio_sequence = atomic_load(&rx_audio_packets);
}

static void publish(app_event_id_t id, uint32_t gen)
{
    /* Integer generation tag, not a borrowed pointer. */
    if (gen == atomic_load(&generation))
        event_bus_publish(id, (void *)(uintptr_t)gen, 0);
}
uint32_t ws_service_generation(void) { return atomic_load(&generation); }
static bool current(const rx_frame_t *packet)
{
    return packet->generation == atomic_load(&generation) &&
           (state == WS_STATE_CONNECTED || state == WS_STATE_AUTHING);
}

/* 仅在连接生命周期边界采样，不逐音频帧扫描堆。按内存能力分别记录最大
 * 连续块与总量，避免把“PSRAM 充足”误当成“内部 RAM 足够建立 TLS”。
 * minimum_free_bytes 是开机以来的低水位；不同阶段快照用于实板判断泄漏/碎片。 */
static void log_connection_memory(const char *phase)
{
    multi_heap_info_t internal, external;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&external, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Connection heap [%s]: internal free=%u largest=%u min=%u; PSRAM free=%u largest=%u min=%u",
             phase, (unsigned)internal.total_free_bytes, (unsigned)internal.largest_free_block,
             (unsigned)internal.minimum_free_bytes, (unsigned)external.total_free_bytes,
             (unsigned)external.largest_free_block, (unsigned)external.minimum_free_bytes);
}

/* 仅在鉴权发送前计算一次，输入就是本次将发送的 token，而非固件默认值。
 * 只记录长度和 SHA-256 指纹；不输出密钥原文/鉴权 JSON，也不改变鉴权结果。
 * ESP-IDF 6.1 在系统启动时初始化 PSA；诊断失败只告警，不阻断原有语音流程。 */
static void log_auth_token_fingerprint(const char *token, const char *source)
{
    uint8_t digest[32];
    size_t digest_len = 0, token_len = strlen(token);
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)token,
                                          token_len, digest, sizeof(digest), &digest_len);
    if (status != PSA_SUCCESS || digest_len != sizeof(digest)) {
        ESP_LOGW(TAG, "Auth token fingerprint unavailable (status=%ld)", (long)status);
        return;
    }
    static const char hex[] = "0123456789abcdef";
    char fingerprint[sizeof(digest) * 2 + 1];
    for (size_t i = 0; i < sizeof(digest); ++i) {
        fingerprint[i * 2] = hex[digest[i] >> 4];
        fingerprint[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    fingerprint[sizeof(fingerprint) - 1] = '\0';
    ESP_LOGI(TAG, "Auth token: source=%s len=%u sha256=%s",
             source, (unsigned)token_len, fingerprint);
}

static void log_transport_error(const char *phase, const esp_websocket_event_data_t *event)
{
    if (!event) return;
    const esp_websocket_error_codes_t *error = &event->error_handle;
    /* 组件 1.8 的 ERROR 可能先于设置 TCP 错误类型；非 TCP 事件的 TLS/errno
     * 字段未初始化，禁止读取。随后 DISCONNECTED 才可能携带有效传输详情。
     * 不输出 data_ptr，避免组件错误文本带出请求头或凭据。 */
    if (error->error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        ESP_LOGW(TAG, "Transport detail [%s]: socket=%d tls=0x%x stack=%d verify=0x%x http=%d",
                 phase, error->esp_transport_sock_errno, error->esp_tls_last_esp_err,
                 error->esp_tls_stack_err, error->esp_tls_cert_verify_flags,
                 error->esp_ws_handshake_status_code);
    } else if (error->error_type != WEBSOCKET_ERROR_TYPE_NONE) {
        ESP_LOGW(TAG, "Transport detail [%s]: type=%d close=%d",
                 phase, error->error_type, event->close_status_code);
    }
}

static void log_server_error(const proto_msg_t *msg)
{
    /* Do not dump JSON: the server may echo auth/input frames. */
    char message[193] = {0}, secret[256];
    size_t len = msg->error_msg_len > 0 ? (size_t)msg->error_msg_len : 0;
    if (len >= sizeof(message)) len = sizeof(message) - 1;
    if (len) memcpy(message, msg->error_msg, len);
    /* 提醒错误可能回显原始请求，其中 to 含企业 Key；只保留错误码。 */
    bool sensitive = credentials_from_handshake || atomic_load(&reminder_mode) || strstr(message, "sk-") || strstr(message, "token") ||
                     strstr(message, "password") || strstr(message, "Bearer");
    const cfg_key_t keys[] = { CFG_KEY_ACOS_TOKEN, CFG_KEY_WIFI_PASSWORD };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        cfg_service_get_str(keys[i], secret, sizeof(secret), "");
        if (secret[0] && strstr(message, secret)) sensitive = true;
    }
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)message[i] < 32) message[i] = ' ';
    /* Codes are bounded and allowed only as plain identifiers. */
    char code[65] = {0};
    int n = msg->error_code_text_len;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        char c = msg->error_code_text[i];
        code[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') ? c : '?';
    }
    ESP_LOGW(TAG, "Server error: code=%d/%s message=%s", msg->error_code, code,
             sensitive ? "[redacted]" : message);
}

/* RX 回调只观察网络音频进度，不负责结束播放。completed != audio.done。
 * 工具/消息阶段由 worker 按 ID 校验后修改 audio_eof；不能在这里让迟到或
 * 被忽略的控制帧撤销当前边界。FIFO/rx_dispatching 保护尚未处理的阶段。 */
static bool observe_control(const proto_msg_t *msg)
{
    atomic_store(&last_control_event, msg->evt);
    if (msg->evt == PROTO_EVT_SPEECH_STARTED && state == WS_STATE_CONNECTED &&
        !atomic_load(&reminder_mode)) {
        response_stream_user_started(&wire_reply);
        atomic_store(&interrupt_event, msg->evt);
        audio_out_request_stop();
        atomic_fetch_add(&speech_epoch, 1);
        atomic_store(&wire_done_turn, 0);
        atomic_store(&rx_audio_last_ms, 0);
    } else if (response_stream_starts_turn(msg, atomic_load(&reminder_mode))) {
        response_stream_next_turn(&wire_reply, atomic_fetch_add(&wire_turn, 1) + 1);
        atomic_store(&wire_done_turn, 0);
        atomic_store(&rx_audio_last_ms, 0);
    } else if (atomic_load(&reminder_mode) && msg->evt == PROTO_EVT_RESPONSE_DONE) {
        atomic_store(&wire_done_turn, atomic_load(&wire_turn));
    } else if (!atomic_load(&reminder_mode)) {
        response_stream_action_t action = response_stream_control(&wire_reply,
            atomic_load(&wire_turn), msg);
        if (action == RESPONSE_STREAM_CANCELLED) {
            atomic_store(&interrupt_event, msg->evt);
            audio_out_request_stop();
            atomic_fetch_add(&speech_epoch, 1);
            atomic_store(&wire_done_turn, 0);
        } else if (action == RESPONSE_STREAM_AUDIO_END) {
            atomic_store(&wire_done_turn, atomic_load(&wire_turn));
        }
        /* 迟到/零等 ack 不应绕过镜像的 ID/窗口校验，再次终止新回答。 */
        if (msg->evt == PROTO_EVT_RESPONSE_TERMINAL &&
            (strcmp(msg->status, "cancelled") == 0 || strcmp(msg->status, "canceled") == 0))
            return action == RESPONSE_STREAM_CANCELLED;
        if (msg->evt == PROTO_EVT_RESPONSE_DONE)
            return action == RESPONSE_STREAM_AUDIO_END;
    }
    return true;
}

/* 状态只输出固定类别，避免扩展字段带入业务正文。 */
static const char *lifecycle_status(const proto_msg_t *msg)
{
    if (!msg->status[0]) return "unspecified";
    if (strcmp(msg->status, "completed") == 0) return "completed";
    if (strcmp(msg->status, "failed") == 0) return "failed";
    if (strcmp(msg->status, "incomplete") == 0) return "incomplete";
    if (strcmp(msg->status, "cancelled") == 0 || strcmp(msg->status, "canceled") == 0)
        return "cancelled";
    return "other";
}

/* 不打印业务正文、工具参数或原始 ID；哈希仅供前后阶段关联。 */
static void log_lifecycle(const proto_msg_t *msg, response_stream_action_t action)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (action == RESPONSE_STREAM_NONE && (uint32_t)(now - lifecycle_log_ms) < 5000) return;
    lifecycle_log_ms = now;
    uint32_t id_hash = 0;
    for (const unsigned char *p = (const unsigned char *)msg->response_id; *p; ++p)
        id_hash = (id_hash ^ *p) * 16777619u;
    ESP_LOGI(TAG, "Response stage: turn=%lu event=%s id_hash=%08lx status=%s has_tool=%d "
             "action=%d phase=%d tool_pending=%d eof=%d decoded=%lu discarded=%lu queue=%u",
             (unsigned long)reply.turn, proto_event_name(msg->evt), (unsigned long)id_hash,
             lifecycle_status(msg), msg->has_tool_call, action, reply.phase, reply.tool_pending,
             reply.audio_eof, (unsigned long)played_frames, (unsigned long)discarded_frames,
             (unsigned)uxQueueMessagesWaiting(rx_queue));
}

static void release_rx_frame(uint8_t *data, size_t len)
{
    if (!data) return;
    free(data);
    atomic_fetch_sub(&rx_bytes, len + 1);
}

static void drop_audio(void)
{
    if ((++rx_drop_count & 31u) == 1u)
        ESP_LOGW(TAG, "Audio backlog limit: dropped %lu incoming frames; connection retained",
                 (unsigned long)rx_drop_count);
}

static void enqueue_rx_frame(rx_frame_t *packet)
{
    if (xQueueSend(rx_queue, packet, 0) == pdTRUE) return;
    release_rx_frame(packet->data, packet->len);
    if (packet->opcode == 2) drop_audio();
    else {
        /* Audio leaves reserved slots. A control-only overflow is a real
         * protocol failure: never silently lose an auth/end boundary. */
        ESP_LOGE(TAG, "Control queue exhausted; recovering session");
        publish(EV_WS_ERROR, packet->generation);
    }
}

static void handle_frame(const rx_frame_t *packet)
{
    if (!current(packet)) return;
    uint32_t queue_ms = (uint32_t)(esp_timer_get_time() / 1000) - packet->received_ms;
    if (queue_ms > queue_wait_max_ms) queue_wait_max_ms = queue_ms;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    apply_platform_interruption_locked();
    if (!current(packet) || (!packet->session_control &&
        packet->speech_epoch != atomic_load(&speech_epoch))) {
        xSemaphoreGive(playback_lock); return;
    }
    if (packet->opcode == 2) {
        /* 解码/I2S 只在 worker；平台打断代次已过滤旧队列。 */
        bool after_tool = reply.tool_pending;
        if (state == WS_STATE_CONNECTED &&
            response_stream_accept_audio(&reply, packet->turn)) {
            audio_out_set_streaming(true);
            if (audio_out_play_opus(packet->data, packet->len) < 0) {
                /* 平台信号可解除正在等待 PCM 空位的写入；这不是解码失败。 */
                if (packet->speech_epoch == atomic_load(&speech_epoch) && !audio_out_stop_requested())
                    publish(EV_WS_SERVER_ERROR, packet->generation);
            } else {
                ++played_frames;
                /* 每回合首帧/工具续流首帧各记录一次，不逐帧打印。 */
                if (played_frames == 1 || after_tool)
                    ESP_LOGI(TAG, "Reply audio accepted: turn=%lu after_tool=%d decoded=%lu",
                             (unsigned long)reply.turn, after_tool, (unsigned long)played_frames);
            }
        } else if (reply_packet_current(reply.turn, packet->turn)) {
            ++discarded_frames;
            if (packet->after_audio_done && discarded_frames == 1)
                ESP_LOGW(TAG, "Audio arrived after final audio.done: turn=%lu rx_seq=%lu; rejected (verify gateway ordering)",
                         (unsigned long)packet->turn, (unsigned long)packet->audio_sequence);
        }
        xSemaphoreGive(playback_lock);
        return;
    }
    proto_msg_t msg;
    proto_result_t result = proto_parse_json((char *)packet->data, &msg);
    if (result != PROTO_OK) {
        if (result != PROTO_ERR_UNKNOWN_TYPE) {
            ESP_LOGW(TAG, "Invalid control frame: parse=%d bytes=%u", result, (unsigned)packet->len);
            publish(state == WS_STATE_AUTHING ?
                    EV_WS_AUTH_FAILED : EV_WS_SERVER_ERROR, packet->generation);
        }
        xSemaphoreGive(playback_lock); return;
    }
    switch (msg.evt) {
    case PROTO_EVT_PROXY_CONNECTED:
        if (state != WS_STATE_AUTHING) break;
        state = WS_STATE_CONNECTED;
        publish(EV_WS_AUTH_OK, packet->generation);
        break;
    case PROTO_EVT_SPEECH_STARTED:
        if (atomic_load(&reminder_mode)) break;
        /* apply_platform_interruption 已处理；不要让 FIFO 中的同一事件再清一次。 */
        break;
    case PROTO_EVT_SPEECH_STOPPED:
        if (atomic_load(&reminder_mode)) break;
        /* 新窗口不继承上一轮残留 PCM；正常已排空时不重复刷新 DMA。 */
        if (!audio_out_is_drained() && audio_out_stop() != ESP_OK) {
            publish(EV_WS_SERVER_ERROR, packet->generation);
            break;
        }
        response_stream_next_turn(&reply, packet->turn);
        response_stream_hold_input(&reply);
        played_frames = discarded_frames = 0;
        publish(EV_WS_SPEECH_STOPPED, packet->generation);
        break;
    case PROTO_EVT_RESPONSE_CREATED:
    case PROTO_EVT_RESPONSE_TOOL:
    case PROTO_EVT_RESPONSE_MESSAGE:
    case PROTO_EVT_RESPONSE_DONE:
    case PROTO_EVT_RESPONSE_TERMINAL: {
        response_stream_action_t action = response_stream_control(&reply, packet->turn, &msg);
        log_lifecycle(&msg, action);
        if (action == RESPONSE_STREAM_AUDIO_END) {
            ESP_LOGI(TAG, "Audio boundary: turn=%lu rx_seq=%lu queue_wait_ms=%lu queued=%u",
                     (unsigned long)packet->turn, (unsigned long)packet->audio_sequence,
                     (unsigned long)queue_ms, (unsigned)uxQueueMessagesWaiting(rx_queue));
            audio_out_set_streaming(false);
        } else if (action == RESPONSE_STREAM_CANCELLED) {
            audio_out_stop();
            publish(EV_WS_RESPONSE_INTERRUPTED, packet->generation);
        } else if (action == RESPONSE_STREAM_FAILED) {
            audio_out_stop();
            /* 模型本轮失败只收尾当前回复；连接级 error 才走错误恢复。 */
            publish(EV_WS_RESPONSE_INTERRUPTED, packet->generation);
        } else if (action == RESPONSE_STREAM_CONTINUED) {
            /* 仅明确的新 response.created 能恢复已排空回合；迟到二进制不能。 */
            response_stream_hold_input(&reply);
            publish(EV_WS_RESPONSE_CONTINUED, packet->generation);
        }
        break;
    }
    case PROTO_EVT_ERROR:
        log_server_error(&msg);
        reply.phase = RESPONSE_DISCARD;
        publish(state == WS_STATE_AUTHING || proto_is_auth_error(&msg) ?
                EV_WS_AUTH_FAILED : EV_WS_SERVER_ERROR, packet->generation);
        break;
    default: break;
    }
    xSemaphoreGive(playback_lock);
}
static void finish_playback_if_drained(void)
{
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (state == WS_STATE_CONNECTED && !atomic_load(&rx_dispatching) &&
        applied_speech_epoch == atomic_load(&speech_epoch) &&
        response_stream_finish_if_drained(&reply, audio_out_is_drained(),
            uxQueueMessagesWaiting(rx_queue) == 0,
            reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)))) {
        ESP_LOGI(TAG, "Playback drained: turn=%lu decoded=%lu discarded=%lu",
                 (unsigned long)reply.turn, (unsigned long)played_frames,
                 (unsigned long)discarded_frames);
        /* 空响应不能算播报成功，避免平台尚未接好语音时静默吞掉提醒。 */
        publish(atomic_load(&reminder_mode) && !played_frames ? EV_WS_SERVER_ERROR :
                EV_WS_RESPONSE_DONE, atomic_load(&generation));
    }
    xSemaphoreGive(playback_lock);
}
/* 快速通道仅处理平台信号；本地 VAD/触屏不能推进 speech_epoch。 */
static void apply_platform_interruption_locked(void)
{
    uint32_t epoch = atomic_load(&speech_epoch);
    if (state == WS_STATE_CONNECTED && applied_speech_epoch != epoch) {
        applied_speech_epoch = epoch;
        response_stream_user_started(&reply);
        esp_err_t err = audio_out_stop();
        ESP_LOGI(TAG, "Platform interruption: event=%s; old playback cleared; microphone continues (epoch=%lu)",
                 proto_event_name(atomic_load(&interrupt_event)), (unsigned long)epoch);
        publish(err == ESP_OK ? EV_WS_RESPONSE_INTERRUPTED : EV_WS_SERVER_ERROR,
                atomic_load(&generation));
    }
}

static void receive_task(void *arg)
{
    (void)arg;
    rx_frame_t packet;
    bool held = false;
    uint32_t stats_ms = 0, stats_packets = 0, stats_generation = 0;
    while (true) {
        xSemaphoreTake(playback_lock, portMAX_DELAY);
        apply_platform_interruption_locked();
        xSemaphoreGive(playback_lock);
        if (held || xQueueReceive(rx_queue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
            bool wait_input = false;
            bool superseded = !packet.session_control && packet.speech_epoch != atomic_load(&speech_epoch);
            xSemaphoreTake(playback_lock, portMAX_DELAY);
            if (!superseded && current(&packet) && packet.opcode == 2 &&
                response_stream_input_held(&reply, packet.turn)) {
                superseded = packet.turn != atomic_load(&wire_turn);
                wait_input = !superseded;
            }
            xSemaphoreGive(playback_lock);
            if (wait_input) {
                /* 只暂持当前一包，其他包仍在原有有界 RX 队列；不持锁等待主任务。
                 * 平台打断会解除 hold 并丢弃旧音频；新 speech_stopped 已到达时也必须
                 * 跳过旧包，让 worker 处理后面的新回合，不能被旧回答堵住。 */
                held = true;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            held = false;
            if (!superseded) handle_frame(&packet);
            release_rx_frame(packet.data, packet.len);
        }
        finish_playback_if_drained();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t gen = atomic_load(&generation), count = atomic_load(&rx_audio_packets);
        if (gen != stats_generation) {
            stats_generation = gen; stats_packets = 0; stats_ms = now; queue_wait_max_ms = 0;
        }
        if (state == WS_STATE_CONNECTED && count != stats_packets && now - stats_ms >= 5000) {
            ESP_LOGI(TAG, "RX audio stats: received=%lu queued=%u gap_max_ms=%lu queue_wait_max_ms=%lu after_eof=%lu",
                     (unsigned long)count, (unsigned)uxQueueMessagesWaiting(rx_queue),
                     (unsigned long)atomic_load(&rx_audio_gap_max_ms), (unsigned long)queue_wait_max_ms,
                     (unsigned long)atomic_load(&rx_audio_after_eof));
            stats_ms = now; stats_packets = count; queue_wait_max_ms = 0;
        }
    }
    vTaskDelete(NULL);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)arg; (void)base;
    uint32_t gen = atomic_load(&generation);
    if (id == WEBSOCKET_EVENT_BEFORE_CONNECT) {
        /* 已创建网络任务/transport，尚未分配本轮 TLS 握手内存。 */
        log_connection_memory("before-tls");
    } else if (id == WEBSOCKET_EVENT_CONNECTED) {
        state = WS_STATE_AUTHING;
        log_connection_memory("connected");
        publish(EV_WS_CONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        state = WS_STATE_DISCONNECTED;
        log_transport_error("disconnected", event);
        release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
        publish(EV_WS_DISCONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        state = WS_STATE_ERROR;
        log_transport_error("error", event);
        log_connection_memory("transport-error");
        publish(EV_WS_ERROR, gen);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = event;
        if (data->op_code != 1 && data->op_code != 2) return;
        if (data->payload_len <= 0 || data->payload_len > 65536 ||
            data->data_len < 0 || data->payload_offset < 0) return;
        if (data->payload_offset == 0) {
            release_rx_frame(frame, frame_size); frame = NULL; received = 0;
            frame_size = data->payload_len; opcode = data->op_code;
            size_t used = atomic_load(&rx_bytes);
            if (opcode == 2 && !ws_rx_audio_fits(uxQueueMessagesWaiting(rx_queue),
                                                used, frame_size + 1)) {
                drop_audio();
                return;
            }
            if (used + frame_size + 1 <= WS_RX_AUDIO_BYTES + WS_RX_CONTROL_BYTES)
                frame = heap_caps_malloc(frame_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!frame) {
                if (opcode == 2) drop_audio();
                else publish(EV_WS_ERROR, gen);
                return;
            }
            atomic_fetch_add(&rx_bytes, frame_size + 1);
        }
        if (!frame || (size_t)data->payload_offset != received ||
            received + (size_t)data->data_len > frame_size) return;
        memcpy(frame + received, data->data_ptr, data->data_len);
        received += data->data_len;
        if (received == frame_size) {
            atomic_store(&rx_dispatching, true);
            frame[received] = 0;
            atomic_store(&last_rx_ms, (uint32_t)(esp_timer_get_time() / 1000));
            rx_frame_t packet = {.data=frame, .len=received, .opcode=opcode, .generation=gen};
            observe_rx_packet(&packet);
            /* 迟到二进制不能撤销 audio.done；窗口由明确的回合事件重开。 */
            if (opcode == 2 && !atomic_load(&reminder_mode))
                response_stream_accept_audio(&wire_reply, atomic_load(&wire_turn));
            bool deliver = true;
            if (opcode == 1) {
                proto_msg_t msg;
                proto_result_t result = proto_parse_json((char *)frame, &msg);
                if (result == PROTO_OK) deliver = observe_control(&msg);
                packet.session_control = result == PROTO_OK &&
                    (msg.evt == PROTO_EVT_PROXY_CONNECTED || msg.evt == PROTO_EVT_ERROR);
            }
            if (atomic_load(&reminder_mode) &&
                (opcode == 2 || atomic_load(&wire_done_turn) == atomic_load(&wire_turn)))
                atomic_store(&reminder_rx_ms, (uint32_t)(esp_timer_get_time() / 1000));
            packet.turn = atomic_load(&wire_turn);
            packet.speech_epoch = atomic_load(&speech_epoch);
            if (deliver) enqueue_rx_frame(&packet);
            else release_rx_frame(packet.data, packet.len);
            frame = NULL; frame_size = received = 0;
            atomic_store(&rx_dispatching, false);
        }
    }
}

static esp_websocket_client_handle_t create_client(const char *uri)
{
    esp_websocket_client_config_t cfg = {
        .uri = uri, .disable_auto_reconnect = true, .network_timeout_ms = 10000,
        .task_stack = 6144, /* 有界 JSON 解析和协议字段副本，避免默认栈过紧。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* WebSocket-level keepalive: send a ping every 20s so server idle timeout
         * does not silently close the connection. PONG timeout 30s before we
         * declare the link dead. (esp_websocket_client v1.8.0) */
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 30,
        .disable_pingpong_discon = false,
        /* TCP-level keepalive (slower safety net). */
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };
    esp_websocket_client_handle_t created = esp_websocket_client_init(&cfg);
    if (created && esp_websocket_register_events(created, WEBSOCKET_EVENT_ANY, on_ws, NULL) != ESP_OK) {
        esp_websocket_client_destroy(created);
        return NULL;
    }
    return created;
}

static void release_credentials(proto_ws_credentials_t *value)
{
    if (!value) return;
    volatile unsigned char *p = (volatile unsigned char *)value;
    for (size_t i = 0; i < sizeof(*value); ++i) p[i] = 0;
    free(value);
}

esp_err_t ws_service_set_credentials(const char *url, const char *token)
{
    if (!client || client_started || state != WS_STATE_DISCONNECTED) return ESP_ERR_INVALID_STATE;
    if ((url == NULL) != (token == NULL)) return ESP_ERR_INVALID_ARG;
    if (url && (!*url || !*token || strlen(url) > PROTO_WS_URL_MAX ||
                strlen(token) > PROTO_AUTH_TOKEN_MAX)) return ESP_ERR_INVALID_SIZE;
    proto_ws_credentials_t *next = heap_caps_calloc(1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) return ESP_ERR_NO_MEM;
    if (url) {
        memcpy(next->url, url, strlen(url) + 1);
        memcpy(next->token, token, strlen(token) + 1);
    } else {
        /* 仅由主任务在平台握手明确关闭时选择；不是网络/HTTP 失败时的兜底。 */
        cfg_service_get_str(CFG_KEY_WS_URL, next->url, sizeof(next->url), ACOS_WS_URL);
        cfg_service_get_str(CFG_KEY_ACOS_TOKEN, next->token, sizeof(next->token), "");
    }
    if (!next->url[0] || !next->token[0]) { release_credentials(next); return ESP_ERR_INVALID_ARG; }
    if (!credentials || strcmp(credentials->url, next->url) != 0) {
        /* URL 改变时重建已停止的 transport，避免组件 set_uri 遗留旧端口/路径。
         * 新对象准备成功后才替换；失败时保留旧对象但调用者不得开始连接。 */
        esp_websocket_client_handle_t replacement = create_client(next->url);
        if (!replacement) { release_credentials(next); return ESP_ERR_NO_MEM; }
        esp_err_t err = esp_websocket_client_destroy(client);
        if (err != ESP_OK) {
            esp_websocket_client_destroy(replacement);
            release_credentials(next);
            return err;
        }
        client = replacement;
    }
    release_credentials(credentials);
    credentials = next;
    credentials_from_handshake = url != NULL;
    return ESP_OK;
}

esp_err_t ws_service_init(void)
{
    if (client) return ESP_OK;
    playback_lock = xSemaphoreCreateMutex();
    rx_queue = xQueueCreateWithCaps(WS_RX_FRAMES, sizeof(rx_frame_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playback_lock || !rx_queue) goto fail;
    char uri[256];
    cfg_service_get_str(CFG_KEY_WS_URL, uri, sizeof(uri), ACOS_WS_URL);
    client = create_client(uri);
    if (!client) goto fail;
    /* 解码与 AFE fetch 同为 5，PCM 输出为 6；编码已移至 core 0/priority 4。 */
    if (xTaskCreatePinnedToCore(receive_task, "ws_rx_audio", 8192, NULL, 5, &rx_task, 1) != pdPASS) goto fail;
    ESP_LOGI(TAG, "Full-duplex tasks: encode=core0/prio4 decode=core1/prio5 pcm=core1/prio6");
#if CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
    ESP_LOGI(TAG, "TLS allocator: PSRAM (SDK); certificate verification enabled");
#else
    ESP_LOGW(TAG, "TLS allocator is not PSRAM; check mbedTLS memory configuration for this board");
#endif
#if CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
    ESP_LOGI(TAG, "WebSocket independent TX lock enabled; full-duplex upload");
#else
#error "Platform interruption requires CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK"
#endif
    ESP_LOGI(TAG, "RX buffer: %u frames, %u KiB Opus budget in PSRAM; %u control slots reserved",
             WS_RX_FRAMES, WS_RX_AUDIO_BYTES / 1024u, WS_RX_CONTROL_RESERVE);
    return ESP_OK;
fail:
    if (client) { esp_websocket_client_destroy(client); client = NULL; }
    if (rx_queue) { vQueueDeleteWithCaps(rx_queue); rx_queue = NULL; }
    if (playback_lock) { vSemaphoreDelete(playback_lock); playback_lock = NULL; }
    return ESP_ERR_NO_MEM;
}
esp_err_t ws_service_connect(void)
{
    if (!client || !credentials) return ESP_ERR_INVALID_STATE;
    if (client_started) return ESP_OK;
    state = WS_STATE_CONNECTING;
    esp_err_t err = esp_websocket_client_start(client);
    if (err == ESP_OK) client_started = true;
    else {
        state = WS_STATE_ERROR;
        log_connection_memory("start-failed");
    }
    return err;
}
esp_err_t ws_service_authenticate(void)
{
    if (!client || !credentials || state != WS_STATE_AUTHING) return ESP_ERR_INVALID_STATE;
    const char *token = credentials->token;
    log_auth_token_fingerprint(token, credentials_from_handshake ?
                              "HTTP/handshake/websocket.token" : "NVS/korvo_cfg/acos_token");
    size_t capacity = strlen(token) * 6 + 32;
    char *auth = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!auth) return ESP_ERR_NO_MEM;
    size_t len = proto_build_auth_frame(auth, capacity, token);
    esp_err_t err = len && esp_websocket_client_send_text(client, auth, len, pdMS_TO_TICKS(1000)) == (int)len
                    ? ESP_OK : ESP_FAIL;
    volatile char *wipe = auth;
    for (size_t i = 0; i < capacity; ++i) wipe[i] = 0;
    free(auth);
    return err;
}
esp_err_t ws_service_disconnect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    bool was_started = client_started;
    state = WS_STATE_DISCONNECTED;
    if (client_started) {
        esp_err_t err = esp_websocket_client_stop(client);
        /* Component 1.8 returns ESP_FAIL if its task already stopped after
         * a transport failure (auto reconnect is disabled). That is success
         * for this idempotent main-task stop, not a reason to loop forever. */
        if (err != ESP_OK && err != ESP_FAIL) return err;
        client_started = false;
    }
    atomic_fetch_add(&generation, 1);
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    state = WS_STATE_DISCONNECTED;
    response_stream_reset(&reply);
    response_stream_reset(&wire_reply);
    atomic_store(&rx_dispatching, false);
    atomic_store(&wire_turn, 0);
    atomic_store(&wire_done_turn, 0);
    atomic_store(&speech_epoch, 0);
    applied_speech_epoch = 0;
    atomic_store(&reminder_mode, false);
    audio_out_stop();
    xSemaphoreGive(playback_lock);
    rx_frame_t packet;
    while (xQueueReceive(rx_queue, &packet, 0) == pdTRUE) release_rx_frame(packet.data, packet.len);
    release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
    rx_drop_count = 0;
    atomic_store(&rx_audio_packets, 0); atomic_store(&rx_audio_last_ms, 0);
    atomic_store(&rx_audio_gap_max_ms, 0); atomic_store(&rx_audio_after_eof, 0);
    /* stop 已等待 transport 关闭；任务栈的最终回收由空闲任务完成，下一次
     * before-tls 快照也要一起比较，不能单凭此处的差值就认定内存泄漏。 */
    if (was_started) log_connection_memory("after-stop");
    return ESP_OK;
}
esp_err_t ws_service_deinit(void)
{
    if (!client) return ESP_OK;
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) return err;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (rx_task) { vTaskDelete(rx_task); rx_task = NULL; }
    xSemaphoreGive(playback_lock);
    esp_websocket_client_destroy(client); client = NULL;
    release_credentials(credentials); credentials = NULL;
    credentials_from_handshake = false;
    vQueueDeleteWithCaps(rx_queue); rx_queue = NULL;
    vSemaphoreDelete(playback_lock); playback_lock = NULL;
    return ESP_OK;
}
ws_conn_state_t ws_service_get_state(void) { return state; }
bool ws_service_get_reply_offer(ws_reply_offer_t *offer)
{
    if (!playback_lock || !offer) return false;
    /* 每轮主循环都会查询；播放忙时留到下一轮，不拖住按键/触屏和超时处理。 */
    if (xSemaphoreTake(playback_lock, 0) != pdTRUE) return false;
    bool pending = state == WS_STATE_CONNECTED &&
        applied_speech_epoch == atomic_load(&speech_epoch) &&
        response_stream_input_held(&reply, atomic_load(&wire_turn));
    if (pending) {
        offer->generation = atomic_load(&generation);
        offer->turn = reply.turn;
    }
    xSemaphoreGive(playback_lock);
    return pending;
}

bool ws_service_accept_reply(const ws_reply_offer_t *offer)
{
    if (!playback_lock || !offer) return false;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    bool accepted = state == WS_STATE_CONNECTED &&
        applied_speech_epoch == atomic_load(&speech_epoch) &&
        offer->generation == atomic_load(&generation) &&
        offer->turn == atomic_load(&wire_turn) &&
        response_stream_release_input(&reply, offer->turn);
    xSemaphoreGive(playback_lock);
    return accepted;
}
esp_websocket_client_handle_t ws_service_get_client(void) { return client; }
esp_err_t ws_service_send_json(const char *text)
{
    if (!client || state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(text);
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(1000)) == (int)len ? ESP_OK : ESP_FAIL;
}
esp_err_t ws_service_send_opus_frame(const uint8_t *data, size_t len)
{
    if (!data || !len || len > 65536) return ESP_ERR_INVALID_ARG;
    if (state != WS_STATE_CONNECTED || atomic_load(&reminder_mode)) return ESP_ERR_INVALID_STATE;
    size_t capacity = 4 * ((len + 2) / 3) + 1, written;
    char *encoded = malloc(capacity), *json = malloc(capacity + 256);
    if (!encoded || !json) { free(encoded); free(json); return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_FAIL;
    if (mbedtls_base64_encode((unsigned char *)encoded, capacity, &written, data, len) == 0) {
        encoded[written] = 0;
        if (proto_build_input_audio_append(json, capacity + 256, encoded, NULL)) err = ws_service_send_json(json);
    }
    free(encoded); free(json); return err;
}
/* UI 事件可能排队；只有当前阶段与网络回合一致才允许改变界面。 */
static bool reply_is(response_phase_t phase)
{
    if (!playback_lock) return false;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    bool matches = state == WS_STATE_CONNECTED && reply.phase == phase &&
        (phase == RESPONSE_DISCARD || reply.turn == atomic_load(&wire_turn)) &&
        applied_speech_epoch == atomic_load(&speech_epoch);
    xSemaphoreGive(playback_lock);
    return matches;
}
bool ws_service_reply_interrupted(void) { return reply_is(RESPONSE_DISCARD); }
bool ws_service_reply_finished(void) { return reply_is(RESPONSE_IDLE); }

void ws_service_log_response_wait(void)
{
    if (!playback_lock || !rx_queue) return;
    /* 主任务超时前取一次一致快照；不发送网络请求，不输出业务内容。 */
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    ESP_LOGW(TAG, "Reply pending: turn=%lu phase=%d tool_pending=%d eof=%d "
             "decoded=%lu discarded=%lu drained=%d queue=%u dispatching=%d wire_done=%d rx_age_ms=%lu",
             (unsigned long)reply.turn, reply.phase, reply.tool_pending, reply.audio_eof,
             (unsigned long)played_frames, (unsigned long)discarded_frames, audio_out_is_drained(),
             (unsigned)uxQueueMessagesWaiting(rx_queue), atomic_load(&rx_dispatching),
             reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)),
             (unsigned long)((uint32_t)(esp_timer_get_time()/1000) - atomic_load(&last_rx_ms)));
    xSemaphoreGive(playback_lock);
}

esp_err_t ws_service_request_reminder(const char *payload)
{
    if (!payload || !payload[0]) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (state != WS_STATE_CONNECTED || reply.phase != RESPONSE_IDLE ||
        atomic_load(&reminder_mode) || !audio_out_is_drained()) {
        xSemaphoreGive(playback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* 提醒没有 speech_stopped。先创建接收回合，再发送，避免快速下行被丢弃。
     * MQTT 消息已由服务层校验/持有，这里不解析或重新序列化。 */
    response_stream_begin(&reply, atomic_fetch_add(&wire_turn, 1) + 1);
    atomic_store(&wire_done_turn, 0);
    played_frames = discarded_frames = 0;
    reminder_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store(&reminder_rx_ms, reminder_started_ms);
    atomic_store(&reminder_mode, true);
    xSemaphoreGive(playback_lock);
    /* 不持 playback_lock 等待网络；接收任务仍能处理快速到达的音频。 */
    esp_err_t err = ws_service_send_json(payload);
    if (err == ESP_OK) ESP_LOGI(TAG, "Reminder MQTT payload forwarded unchanged; waiting for Opus audio");
    return err; /* 失败由主任务断连清理，不能自动重播可能已发送的请求。 */
}

bool ws_service_reminder_timed_out(void)
{
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    bool expired = atomic_load(&reminder_mode) && reminder_playback_expired(
        (uint32_t)(esp_timer_get_time() / 1000), reminder_started_ms,
        atomic_load(&reminder_rx_ms),
        reply.audio_eof && !reply.tool_pending &&
        reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)));
    xSemaphoreGive(playback_lock);
    return expired;
}
