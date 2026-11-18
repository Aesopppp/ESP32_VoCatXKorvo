# 设备 MQTT 与提醒播报接入（2026-11-17）

## 本次范围与当前状态

连接层依据 DEVICE-MQTT-INTEGRATION.md v1.4（2026-11-09）；提醒会话依据
DEVICE-TASK-BROADCAST-CONTRACT.md（2026-11-15）的任务编号、首帧 auth 和回执约定。
保留本工程纯 C 服务、主任务、事件总线、音频与 UI 架构，使用现有 HTTP 握手结果。

**当前源码包含 MQTT 连接、状态上报、持久化任务排队、独立 WS 提醒会话、成功/失败回执。**
提醒适配默认启用。空闲时用 `auth` 携带 `device_id`、`api_key`、`task_ids`，
由平台按编号取内容并返回现有 raw Opus 音频；不再把 MQTT JSON 原文转发给 WS。
忙时等当前对话结束，多条待播编号合为一次会话；不使用本地 TTS。

本轮按用户要求**不改动既有 token 到期/续期逻辑**，也不新增提醒专用的续期策略。
仍调用现有 HTTP 凭据准备接口，未改变握手服务或其配置。

2026-11-17 已嵌入平台 CA，用户确认 MQTT 连接测试通过。
这表示连接层已可用，不等于已验证提醒语音的完整播报。
其他环境缺少 CA 时，构建仍明确禁用 MQTT 网络连接，语音、触屏和 Wi-Fi 可继续工作。

之前附件 DEVICE-MQTT-INTEGRATION (1).md 是 v1.1；它的 status/task 字段与当前
实现一致，但没有后来增加的 TLS、WS token 有效期和播放 ACK。
目前继续保留已对接的 v1.4 连接规则，避免退回旧平台接口。
上下行联调示例和验收日志见 [MQTT 上下行联调](device-mqtt-checklist.md)。

## 模块和责任

| 文件 | 责任 |
| --- | --- |
| main/device_mqtt_protocol.c/.h | 纯 C 消息校验、分片重组、成功记录、退避和 SUBACK 判定 |
| main/device_mqtt_storage.c/.h | 紧凑持久格式、旧结构体兼容解码；无 NVS/RTOS 依赖 |
| main/device_mqtt_service.c/.h | MQTT 生命周期、独立 worker、有限队列、状态上报、NVS 和 ACK |
| main/device_handshake_service.c/.h | 身份/网络代次、HTTP 返回快照；语音 token 续期不随意重启 MQTT |
| main/app_main.c | 空闲门禁、批量领取、临时会话、超时和成功/失败归还 |
| main/proto_service.c/.h | 普通语音鉴权、提醒专用首帧 auth 的构造与校验 |
| main/ws_service.c/.h | 提醒上下文、原有 Opus 播放和响应边界、有效音频完成判定 |
| main/certs/device_mqtt_ca.pem | 平台 MQTT 根证书，获得后放在这里；与 443 的公共证书包不同 |
| main/device_mqtt_local_config.example.h | 私有开关示例；设为 0 可只接收任务、不建立提醒会话 |
| scripts/check_device_mqtt.py | 直接编译运行生产协议和服务代码的主机回归 |

旧 mqtt_service、mqtt_topics、acos_reminder_service、reminder_message 未加入编译。
它们的旧主题、身份和任务格式不适合本次新协议，不能与新模块同时启动。
组件使用 espressif/mqtt 1.1.0，依赖锁保留原有组件版本。

## 连接和鉴权

1. Wi-Fi 获得 IP，现有 HTTP 服务取得配置。
2. MQTT 独立复制 endpoint、client_id、username、password 和两个 topic。
3. 仅接受 mqtts 地址。显式使用 MQTT 3.1.1、keepalive 60 秒、clean session。
4. TLS 使用平台 CA 校验证书链和主机名，不跳过校验、不改成明文。
5. 每次连接重新订阅握手返回的 subscribe_topic，并立即发送首次 status。
6. 待机仍保留 MQTT；现有“待机断开语音 WS”策略不变。
7. HTTP 单纯更新 WS token 时保留健康 MQTT；断网、配网、身份配置重读
   或 HTTP 永久错误时，停止使用旧 MQTT 配置，等待新有效结果。

日志不输出 MQTT password、PSK、任务 content 或完整握手响应。
原先用户明确开启的 DEV ONLY websocket.token 日志未在本次更改，
正式固件应关闭它；它不代表 MQTT password。

### CA 和时间的注意事项

把平台 2026-11-09 后重新签发、包含 CA/keyCertSign 用途的 PEM 证书保存到
main/certs/device_mqtt_ca.pem，再运行 idf.py reconfigure 和 idf.py build。
公有根证书包不能替代此 CA；不应从正在连接的未知服务端临时接受证书作为信任根。

本工程继承的 sdkconfig 关闭了 CONFIG_MBEDTLS_HAVE_TIME_DATE。
当前仍校验证书链/签名和主机名，但不能据此声称已由 TLS 库验证证书有效期。
HTTP 成功会设置系统时间；如量产要求 TLS 严格有效期校验，应在已有 HTTPS 握手
之前增加可靠校时流程，再统一打开该配置并验证冷启动。此次未改变全局 TLS 时间策略，
避免让设备在尚未校时的开机阶段无法完成既有 HTTPS 握手。

## 状态、SUBACK 与重连

- status 主题逐字使用 publish_topic，不发送 LWT/retained 旧状态。
- 上报固件版本、板型、当前 IP、RSSI；省略不可靠的 ts。
- 正常每 60 秒一次，QoS1、retain=false；至多一条等待 PUBACK 的 status。
- `Status queued` 表示进入客户端发送队列；`Status PUBACK` 表示 broker 确认接收。
  后者不代表平台业务数据库一定已更新，平台在线页仍需联调核对。
- SUBACK 检查原始一个返回字节：0 是 QoS0，1 是 QoS1，128/非法值报 ERROR。
  SUBACK 被拒保持连接，不因配置错误反复重连；日志打印实际订阅主题。
- 普通失败从约 1 秒指数退避到最多 60 秒，带约 ±20% 抖动，上限仍为 60 秒。
- CONNACK4/5 最多连续拒绝五次，然后等待真实用户唤醒或重启。
  Wi-Fi 自动重连、任务到达和 WS token 自动续期不解除此限制。
- 客户端停止/销毁和 TLS 建连由 worker 执行，不在主任务或 MQTT 回调中执行。

## 接收与持久化

任务必须是含 `task_id`、`kind=reminder`、`content`、`fire_at` 的 JSON 对象。
`task_id` 是纯 ASCII 数字字符串，保留前导零、不转整数；最长 128 字节，
超长明确拒绝而不截断。整包最长 4096 字节。`content` 只校验，不存储为播报内容；
`fire_at` 校验 UTC 格式，不按它二次等待，也不丢弃平台补派的迟到任务。
拒绝 retained、其他主题、畸形/重复键、NUL、错误分片和超长消息。

任务容量总计 8 条（包括正在处理和等待失败回执的任务）。同一排队/处理中的
编号不重复入队。队满明确记录日志，不生成成功 ACK；依赖平台处理未确认投递。

使用独立 NVS namespace `device_tasks`：

| Key | 内容 | 恢复方式 |
| --- | --- | --- |
| `pending_v1` | 版本、设备身份、最多 8 个编号、收到的 Unix 秒时间、等待/失败回执状态 | 重启恢复；未同步系统时间时收到时间可为 0；顺序以入队为准 |
| `played_v1` | 最近 32 条成功编号及环形写入位置 | 重复任务只补回执，不重新起会话 |

`Task queued` 仅是 RAM 已接受；`Task queue persisted` 才表示已提交 Flash。
worker 批量写入，成功后才允许主任务领取。保存失败保留 RAM，5 秒后重试，
在保存成功前暂停新播报和任务 ACK。重启读取损坏则阻止任务播报并报错。
成功时先提交成功记录，再删除 pending；两次提交之间掉电，启动会对账剔除已成功项。
旧成功记录里的非数字编号可读取，但不再发送给新版 ACK 接口。

2026-11-17 存储修复：保留上述键名，新值使用 `MQH2` / `MQP2` 标记的 v2 紧凑格式，
按字符串实际长度保存，不再写满 128 字节的空 ID 槽和结构体 padding。
新固件可读取旧结构体记录，校验后由 worker 原位提交新格式；写成功前不删除旧值。
迁移/启动对账写失败仍可重试，成功前继续暂停 ACK 和新播放。
仅旧版固件不认识 v2 数据，回退固件时须保留本次存储兼容代码，不能直接假定双向兼容。

## 提醒会话与播放边界

`DEVICE_MQTT_FORWARD_TASK_TO_WS` 默认 1；已有私有配置若显式设为 0，应改为 1。
宏名为兼容配置保留，含义已变成“启用任务编号提醒会话”，不是原文转发。

1. 网络已就绪、未配网、没有活跃会话且处于 WAIT_WAKE 时，批量领取所有可播编号。
2. 若用户先唤醒，尚未领取的批次退回队列，挂断后与新任务重新合并。
3. 复用现有 HTTP 凭据准备和 WS 建连流程；首帧如下，普通对话 auth 不变：

   ```json
   {"type":"auth","api_key":"<websocket.token>","device_id":"<device_id>","task_ids":["42","43"]}
   ```

4. `__proxy_connected__` 后直接等待下行，跳过 LISTENING，不启动麦克风上传。
   提醒期间 AFE 用 LITE（关闭 WakeNet，避免自唤醒），Wi-Fi 切到音频性能模式。
   同时关闭上传 FIFO 写入，避免无消费者时循环积压、丢帧；不停止 AFE 和共享 I2S 时钟。
   结束并回到 WAIT_WAKE 后恢复 FIFO 门禁，普通对话的连续上传照常运行。
5. 平台按编号取权威内容并播报；设备不发送 `content`、`response.create` 或额外 TTS 请求。
6. 继续使用当前二进制 raw Opus、16 kHz 单声道播放链路和响应 ID 校验。
   `response.done` 只表示响应阶段完成，**不能代替音频结束**；平台必须在音频发完后发送
   `response.audio.done` 或 `response.output_audio.done`。
7. 收到有效解码音频、结束边界且本地 PCM 排空后，归还成功结果并回待机；
   恢复 WakeNet 和 Wi-Fi 省电，MQTT 保持连接。

**判据需双方确认**：契约 §6.1 说收到音频即可确认，§6.4 又要求中途断流报失败。
本实现采用与原播放器一致的保守判据：有效音频 + 音频结束边界 + 本地排空才确认。
这仍不代表用户实际听见；无音频、解码失败或空响应均不确认。

## 成功/失败回执和重试

主题 `devices/{device_id}/ack`；QoS 1、retain=false：

```json
{"task_ids":["42","43"],"audio_received":true}
```

失败发送同一批编号和 `audio_received:false`，不再发送旧的仅 `task_id` 回执。
成功/失败结果先持久化，再发 ACK。失败状态保留至 broker PUBACK；
成功记录在重连/重启时可重复补 ACK。PUBACK 只确认 broker 接收，不代表平台数据库已更新。

| 情况 | 设备行为 |
| --- | --- |
| 建连/鉴权失败、无首帧或流内 30 秒无有效音频进展 | 关闭该提醒会话，回 false，等待平台重新投递，不用本地计时器自动重播 |
| 服务端错误、取消、解码/输出错误，或用户结束提醒 | 回 false，等待平台重投 |
| Wi-Fi 已离线 | 保留任务及失败回执，真实网络恢复后先补 false ACK，再恢复待播；不按 5 秒循环重播 |
| 已确认成功的编号再次收到 | 只补 true ACK，不起新会话 |

沿用提醒保护时限：接收流总计最多 300 秒，收到音频边界后的排空总时限 360 秒，
等待凭据等整个提醒流程最多 360 秒。普通对话的超时、连续上传与平台打断策略保持原样。
平台负责失败投递最多三次；设备没有额外的无音频重播计数器。

边界：持久化前掉电仍可能丢失刚收到的 RAM 任务；物理播放结束到成功记录提交之间
掉电仍可能重播。现有协议只有任务编号，没有每次投递的 attempt_id，无法完全区分
“失败后的 QoS 旧重投”和“平台安排的新尝试”，也不能保证跨设备/平台严格恰好一次。

当前 NVS 分区仍为 20 KiB，与原有配置共用。旧格式即使只有一个短 ID，
也写入约 4.2 KiB 成功历史、1.3 KiB 待处理记录，更新时还需要新副本和 GC 空间。
新格式在 16 字节设备 ID、一个两位任务 ID 时，历史仅 56 字节、待播记录 35 字节
（不含 NVS 元数据）。保留 32 条历史、8 条待播和 128 字节 ID 上限；极长 ID 仍占较多空间，
不能据此保证任意其他配置占用下都可写入。失败日志增加 key、bytes、available_entries，
不自动擦除其他配置。主机有限容量模拟不代替实板 Flash/GC 和掉电测试。

对已经出现保存失败的旧固件，触摸待机或重新唤醒不会释放 NVS。
升级重启后只能恢复已落盘的数据；若上一提醒的成功标记尚在 RAM，保留的 pending 可能
再次尝试播报。不要为消除日志而全片擦除；先保留 NVS 升级并检查迁移、回执日志。

## 无声诊断

`Reply audio accepted` 只证明 Opus 可解码，`audio_received=1` 不证明用户听到内容。
新日志在每 5 秒的 Playback stats 输出累计 `pcm_peak`、`pcm_mean_abs`，在音频 EOF
输出 `PCM signal at EOF: decoded_ms=... peak=... mean_abs=... nonzero=.../... volume=...`，
因此很短的流也有诊断。幅度统计位于解码后、音量增益前，不改变播放完成/ACK 判据。

- peak=0 且 nonzero=0：已解码内容全零；平台需检查合成、编码及实际发送内容。
- 峰值很小：可能近静音，结合普通问答同音量的统计对比，不以任意阈值认定“已听到”。
- 幅度明显且音量非零但仍无声：继续核对输出、功放、回采和实际接线。
- `Audio arrived after final audio.done`：结束边界后仍有音频到达，仍按既有规则拒收；
  平台需确认最后一个 Opus 包发送完毕再发 audio.done，本次不放宽边界掩盖问题。

## 验证与实板联调

主机运行：python scripts/check_device_mqtt.py --cc <本机TCC路径>。
同时运行 check_handshake.py、check_device_auth.py、check_audio.py。
主机测试覆盖生产解析和服务函数，模拟 MQTT/NVS/时钟，不能代替真实 TLS、
Wi-Fi 掉线时序、音频效果、Flash 写入耗时和双连接峰值内存测试。

先验证连接、状态页面、SUBACK、普通任务接收、重复任务、断网恢复；
不要在公共平台故意发布别人的主题。ACL 负向测试应在授权的测试环境完成。
再验证实际完整播报、失败 false ACK、忙时批量、重启恢复及平台回执页面。
验收步骤、关键日志与当前主机结果见 [MQTT 上下行联调](device-mqtt-checklist.md)。

本次没有烧录设备，没有向 GitHub 提交或推送。

### 初次开发时的验证记录（2026-11-10，未嵌入 CA）

- MQTT 生产协议：4787 项检查通过。
- MQTT 生产服务模拟：110 项检查通过，包括 NVS 提交失败、重复任务、
  ACK 超时补发、迟到 PUBACK、身份/网络代次、订阅失败和鉴权限次。
- 缺少 CA 保护：2 项；语音协议未确定保护：19 项，均通过。
- HTTP 握手：1066 项；动态语音鉴权：131 项，均通过。
- 既有音频回归：响应流 413 项、输入上传 97874 项、交接策略 341 项、
  主任务交接 268 项，均通过。
- ESP-IDF 6.1 / ESP32-S3 完整构建成功；固件大小 2,555,760 字节，
  应用分区剩余 589,968 字节（约 19%）。**该固件未嵌入 CA，MQTT 运行时关闭。**
- 额外交叉编译了 DEVICE_MQTT_HAS_CA=1 的完整服务分支，通过；
  只在 work 生成验证对象，没有虚构/嵌入测试 CA，也没有替换正式固件。
- 固件 SHA-256：
  80794f86f5f15d8e2c54b7b35faddc9eec9c93ec35cf1c9f70b7e92e33f48b20。
- 构建与配置日志在 work/mqtt-20261110；MQTT 测试日志在 work/device-mqtt。
- 与开发前备份逐文件核对：音频采集/输出、WakeNet、板级驱动、屏幕/触屏/UI、
  WebSocket 协议实现及本地握手私有配置未被本次修改；应用主任务仅新增 MQTT 协调入口。

MQTT 服务和客户端任务各使用 6 KiB 栈；任务原文、去重记录和配置副本在 PSRAM，
MQTT outbox 上限 16 KiB。任务栈、网络客户端和 TLS 的实板峰值需在 CA 到位后，
结合原语音/UI 同时运行时检查内部堆余量；静态编译不能证明峰值内存充足。
