# 语音平台对接速查

适用工程：`D:\ESP32_VoCatXKorvo`，按当前固件行为整理（2026-11-13）。本文只描述普通语音对话。

## 1. 鉴权

设备连接 HTTP 握手返回的 `websocket.url`，发送文本消息：

```json
{"type":"auth","token":"<HTTP握手返回的websocket.token>"}
```

平台鉴权通过、语音服务就绪后返回：

```json
{"type":"__proxy_connected__"}
```

## 2. 设备上传语音

WebSocket 文本消息；每包为 16 kHz、单声道、20 ms 的 Opus，码率 24 kbps：

```json
{"event_id":"event_123456_abcdef","type":"input_audio_buffer.append","audio":"<单个Opus包的Base64>","encoding":"opus"}
```

平台按 `Base64 → Opus → PCM` 解码，并自动判断发言结束、生成回复。当前设备不另发 `input_audio_buffer.commit` 或 `response.create`。

## 3. 平台下发回复

按以下顺序，在同一连接发送；JSON 使用文本消息，音频使用二进制消息：

```text
① 用户开始讲话：{"type":"input_audio_buffer.speech_started"}
② 用户讲话结束：{"type":"input_audio_buffer.speech_stopped"}
③ 回答音频：Binary <Opus包1>
④ 回答音频：Binary <Opus包2> ……
⑤ 音频发送完成：{"type":"response.audio.done","response_id":"resp_001"}
```

- **必须先发 `input_audio_buffer.speech_stopped`，再发回答音频**；同一次发言结束不要重复发送。
- 下行按现有约定使用 **16 kHz、单声道、60 ms/包的原始 Opus**，建议 24 kbps；一个二进制消息放一个完整 Opus 包。
- 不加 JSON、Base64、自定义包头；不发送 PCM、WAV、MP3 或完整 Ogg 文件。
- 当前不支持 `response.audio.delta` 内嵌 Base64 的音频格式。
- `response.created` 不能代替 `speech_stopped` 启动第一轮回复；`response.done` 不能代替正常音频结束事件 `response.audio.done`。
- 若发送 `response.created`，后续同一响应的 `response_id` 应与其 `response.id` 一致。
- 每条 JSON、每个 Opus 包使用完整且不做 WebSocket 协议分片的消息；不要直接拼接多个 JSON 或独立 Opus 包。

## 4. 打断

监听与播放期间连续上传现有 Opus，不门控、不填零，不发送 response.cancel。

```json
{"type":"input_audio_buffer.speech_started"}
```

收到上述平台事件后，设备立即停止本地播放、清空旧 Opus/PCM 队列；上传任务继续。服务端自己停止上一轮模型输出，设备不请求取消、不等待取消确认。平台发来 speech_stopped 后，设备等待新一轮回答音频。

若平台另外发送取消边界，也按停播清队列处理：

```json
{"type":"response.done","response":{"id":"resp_001","status":"cancelled"}}
```

response.cancelled 同样处理；无活动回复的 ack 忽略，可识别的旧响应 ID 不影响新回答。每轮只有一个最终 response.audio.done；收到后关闭接收窗口，已缓存尾音播完后回监听。后续迟到二进制不能重开窗口。

error 按现有鉴权/业务错误分流，不是音频完成边界；普通未知事件忽略。正常打断不重连，真实断网、鉴权失败、输入失败或长期无回复仍保留恢复。

supports_overlapping_speech=False 由服务端配置。本次保留现有 Opus，不改为截图中的 PCM 传输。无 ID 的 raw Opus 无法识别被服务端混入新轮次的旧回答；平台须保证同一连接事件和音频的顺序。实现和验证说明见 [平台打断](platform-barge-in.md)。

## 5. 联调检查

- 确认上传音频收到并成功解码。
- 确认第一包回复音频前已发送准确的 `input_audio_buffer.speech_stopped`。
- 记录回复的二进制包数，最后发送 `response.audio.done`；同时正常响应 WebSocket Ping/Pong。
- 平台打断应出现 Platform interruption / microphone continues；不应再出现取消确认等待或预录音补传。
