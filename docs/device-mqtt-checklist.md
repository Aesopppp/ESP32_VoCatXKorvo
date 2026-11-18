# MQTT 上下行与提醒播报联调

适用工程：ESP32_VoCatXKorvo。沿用已对接的 v1.4 / MQTT 3.1.1 / TLS。
2026-11-17：用户确认 MQTT 连接测试通过；本轮按 DEVICE-TASK-BROADCAST-CONTRACT.md
实现首帧 auth 携带任务编号。新的语音播报链路尚未实板验收。
token 到期/续期沿用现有实现，本轮未改动。

## 上行状态

主题直接使用 HTTP 握手的 `mqtt.publish_topic`。
连接后立即上报，此后每 60 秒上报，QoS 1、retain=false。
待机不停止 MQTT；没有网络时不积压状态消息。

```json
{
  "firmware_version": "1.0.0-vocat",
  "board": "esp-vocat-v1.2",
  "ip": "192.168.1.23",
  "rssi": -58
}
```

字段均来自当前固件/网络状态，示例 IP 和 RSSI 不写入配置；不发送不可靠的 `ts`。
日志 `Status queued` 是客户端已入队，`Status PUBACK` 是 broker 已接收。
平台在线时间是否更新，还需核对平台页面或服务端记录。

## 下行任务

主题直接使用 HTTP 握手的 `mqtt.subscribe_topic`。每次重连重新订阅。
服务端发送 QoS 1、retain=false；不同任务使用不同 `task_id`。

```json
{
  "task_id": "42",
  "kind": "reminder",
  "content": "该吃午饭了",
  "fire_at": "2026-11-17T04:00:00.000Z"
}
```

- `SUBACK granted`：订阅获准；`SUBACK rejected`：未订阅成功。
- `Task queued`：任务已校验并进入 RAM；`Task queue persisted`：待播队列已落 NVS。
- `task_id` 必须是纯数字字符串（含前导零也不转整数）；非法或超长编号拒绝，不截断。
- `content` 是冗余文本，设备不存储为播报内容、不用本地 TTS；平台按编号取权威内容。
- 重复的排队中/处理中任务不再入队；已播报任务不再播放。
- 最多 8 条未完成任务、收到时间及状态保存在 NVS，重启恢复；容量包括处理中的任务。
- 最近 32 条成功编号保存在 NVS，重启继续去重，并可补发成功回执。
- `fire_at` 仅校验 UTC 格式；不拒绝平台补派的迟到任务。
- 队满/保存失败记录原因，不虚报成功。普通对话期间只排队，挂断后批量处理。

## 提醒会话

当前 `DEVICE_MQTT_FORWARD_TASK_TO_WS=1`。如果本地私有配置显式设成 0，需要改为 1。
空闲时建立独立 WS，`ws_service_prepare_reminder()` 准备上下文，首条消息如下：

```json
{
  "type": "auth",
  "api_key": "<HTTP握手返回的websocket.token>",
  "device_id": "<本设备号>",
  "task_ids": ["42", "43"]
}
```

平台发 `__proxy_connected__` 后主动返回现有二进制 raw Opus 音频。
不需要先发 `speech_stopped`；设备不进入监听，不上传麦克风，不转发原 MQTT JSON。
音频发完后发 `response.audio.done` 或 `response.output_audio.done`；
`response.done` 本身不能结束音频。若使用 response_id，相关边界必须匹配当前响应。

预期日志顺序（字段是示意，不是本次实板记录）：

```text
Task queued: id=42 ... voice_adapter=1; persistence pending
Task queue persisted: count=1
Reminder batch ready: task_count=1
Reminder selected: task_count=1; connecting without microphone upload
Reminder auth sent: device_id=... task_count=1 (token redacted)
Reminder session authenticated; waiting for agent audio: task_count=1
Reply audio accepted: turn=1 ... decoded=1
Audio boundary: turn=1 ...
Playback drained: turn=1 decoded=... discarded=...
Reminder result: task_count=1 audio_received=1 network_wait=0
Task ACK queued: count=1 audio_received=1
Playback ACK PUBACK: ... task_count=1
```

## 播出确认与失败回执

主题：`devices/{device_id}/ack`；QoS 1、retain=false。

```json
{"task_ids":["42","43"],"audio_received":true}
```

只有实际收到并解码有效音频、收到结束边界、播放缓冲排空且成功记录持久化后才发送 true。
这采用契约 §6.1/§6.4 的保守解释，需联调双方确认；不等于用户实际听到了。
30 秒无首帧/流内无进展、取消、连接/解码错误发送：

```json
{"task_ids":["42","43"],"audio_received":false}
```

失败先持久化，联网时发送；PUBACK 后释放已确认的失败记录。无音频失败不自行重播，
等平台重新投递；Wi-Fi 离线则保留任务，到真实网络恢复后补回执并恢复处理。
PUBACK 不等于平台数据库已经采纳，需核对设备归属、`device_ack_at` 和 `status`。

## 最小验收

1. 待机观察两次 `Status PUBACK`，核对平台在线时间更新。
2. 待机下发真实属于本设备的任务编号，核对首帧 auth、实际听到的内容、音频边界及 true ACK。
3. 同 ID 重复下发，只出现一次提醒会话；成功后再次下发只补 ACK。
4. 对话中下发两条新任务，当前对话不被打断；挂断后一次 auth 带两个编号，由平台合并。
5. 等 `Task queue persisted` 后重启，确认未播任务恢复；成功任务重启后不重复播。
6. 平台不回音频，确认约 30 秒退出、false ACK；等待期间设备不自行循环重播。
7. 播放中断网，确认不回 true；恢复网络后检查持久化任务、失败回执及后续处理。
8. 测试保存失败/满队列日志、正常对话多轮上传和平台打断，检查声音及 UI 状态。

以上实板项均待新固件烧录后执行；原 MQTT 连接测试不替代它们。

主机回归：`python scripts/check_device_mqtt.py --cc <本机 TCC 路径>`。
测试使用生产协议和服务代码，模拟时钟、MQTT、NVS；不能代替真实平台和扬声器测试。

2026-11-17 本轮主机检查：协议 4765 项、服务 278 项、缺 CA 保护 2 项、
关闭提醒开关保护 55 项、动态鉴权 154 项、HTTP 握手 1066 项通过；
WS 平台打断/提醒 202 项、主任务提醒生命周期 29 项及原有音频回归通过。
测试调用生产代码；不证明真实网络时序、NVS 容量、音量或用户听感。
测试日志位于 `work/device-mqtt/`、`work/device-ws-auth/`、`work/speech-handoff/`；
本轮构建与交付记录位于 `work/task-broadcast-20261117/`。

ESP-IDF 6.1 / ESP32-S3 构建已通过；固件 `build/vocat_korvo_agent.bin`
为 2,592,112 字节，应用分区剩余 553,616 字节（约 18%），已包含平台 CA。
SHA-256：`3c1ff6805169a66964163e017cfdd4fb48cf5be8ca2bd3d38f81400462af92b7`。
普通构建的 Windows cmd 包装层在 ldgen 阶段挂起；从当前 build.ninja 生成临时
direct.ninja，只去掉该命令的包装层后完成构建，未更改 SDK、编译选项或原构建文件。
本轮未烧录、未提交或推送 GitHub；实板声音、掉电恢复和平台数据库结果仍待验证。

## 2026-11-17：任务 21 后的存储修复与无声诊断

实板已观察到：任务 21 只接纳 6 个音频包便收到 audio.done，之后仍有包到达；
用户确认全程无声。播放结果保存报 `ESP_ERR_NVS_NOT_ENOUGH_SPACE`，成功 ACK 尚未发送。
反复触摸待机/唤醒不释放 NVS，因此每约 5 秒保存重试继续失败，后续任务也被暂停。

本次设备侧调整：

- 任务历史/队列改为 v2 紧凑存储，兼容读入旧结构体数据，写成功前保留旧记录。
- 保存失败日志附带键名、写入字节数和 NVS 可用 entry 数；读入后的迁移/对账可重试。
- 提醒期间关闭无人消费的麦克风上传 FIFO，退出恢复；AFE 和共享 I2S 时钟保持运行。
- 增加 `PCM signal at EOF`，记录解码时长、峰值、平均绝对幅度、非零样本数、音量。
  不通过随意延长等待或放宽 audio.done 边界来接纳尾随包。

烧录后重点检查：

1. 保留 NVS 升级，等待 `Legacy ... loaded; compact storage migration pending`（存在旧值才打印）。
2. 检查不再循环保存失败，任务完成后出现 `Task ACK queued` 和 `Playback ACK PUBACK`。
3. 旧固件失败前仅保存在 RAM 的成功状态无法跨重启恢复，旧 pending 可能再尝试播放一次。
4. 下发新任务，提供从 `Reminder auth sent` 到 `PCM signal at EOF`、`Playback drained` 的日志，
   并记录实际听到的内容。peak=0/nonzero=0 表示解码全零；明显幅度但无声需继续查输出。
5. 若仍有 `Audio arrived after final audio.done`，平台需核对发送顺序和尾随包内容。
6. 普通触摸唤醒、问答、平台打断、触摸休眠各验证一次，确认提醒结束后恢复上传。

旧固件不能读取 v2 值；需要回退时保留存储兼容代码，不能依赖旧程序直接恢复新格式。

本次主机检查通过：MQTT 协议 4765 项、紧凑存储 1197 项、服务 2370 项，
缺 CA 保护 2 项、关闭提醒开关 56 项；HTTP 1066 项、语音鉴权 154 项通过。
音频回归通过，包括新增 FIFO 43 项、幅度/短流摘要 16 项、提醒生命周期 32 项。
有限容量模拟覆盖旧格式空间不足、新格式迁移、提交失败、两键之间重启和 200 轮更新；
不代替真实 NVS 页分配/垃圾回收、Flash 断电行为或声音测试。
本次验证记录位于 `work/task-storage-20261117/`。

本次 ESP-IDF 6.1 / ESP32-S3 编译、链接及镜像容量检查通过。新固件为
`build/vocat_korvo_agent.bin`，2,595,168 字节，应用分区剩余 550,560 字节（约 18%）。
SHA-256：`514210b201a6f6c02ecc0bbd2fbac110a92fbe9b6e728c50be71d94534140c4f`。
使用当前生成的构建图，仅绕过已知的 Windows ldgen 命令包装层；SDK、编译参数和分区未改。
未烧录、未推送 GitHub；新固件的旧任务恢复、真实出声和回执仍需实板确认。
