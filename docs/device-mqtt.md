# 设备 MQTT 接入（2026-11-10）

## 本次范围与当前状态

依据桌面的 DEVICE-MQTT-INTEGRATION.md v1.4（2026-11-09）实现。
保留本工程纯 C 服务、主任务、事件总线、音频与 UI 架构，使用现有 HTTP 握手结果。

**当前源码包含 MQTT 连接、状态上报、任务接收与去重、持久化已播报记录、回执及重连。**
用户已确认“请求语音的协议还未确定”，因此自动请求提醒语音默认关闭。
收到任务会校验并进入有界待处理队列，不猜造 TTS 请求、不直接把 content 当成音频，
也不会在“收到消息”时就回报已播报。

另一个联调前提是平台新版 MQTT CA。开发时尚未取得该文件；
缺少 CA 的构建明确禁用 MQTT 网络连接，语音、触屏和 Wi-Fi 可继续工作。
不能把此构建描述为已在实板跑通 MQTT 或提醒播报。

## 模块和责任

| 文件 | 责任 |
| --- | --- |
| main/device_mqtt_protocol.c/.h | 纯 C 消息校验、分片重组、已播报集合、退避和 SUBACK 判定 |
| main/device_mqtt_service.c/.h | MQTT 生命周期、独立 worker、有限队列、状态上报、NVS 和 ACK |
| main/device_handshake_service.c/.h | 身份/网络代次、HTTP 返回快照；语音 token 续期不随意重启 MQTT |
| main/app_main.c | 待机领取任务、已有 WS 播放接口的关闭状态适配入口、成功/失败归还 |
| main/certs/device_mqtt_ca.pem | 平台 MQTT 根证书，获得后放在这里；与 443 的公共证书包不同 |
| main/device_mqtt_local_config.example.h | 私有配置入口示例；尚未确认语音协议时保持默认关闭 |
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
- SUBACK 检查原始一个返回字节：0 是 QoS0，1 是 QoS1，128/非法值报 ERROR。
  SUBACK 被拒保持连接，不因配置错误反复重连；日志打印实际订阅主题。
- 普通失败从约 1 秒指数退避到最多 60 秒，带约 ±20% 抖动，上限仍为 60 秒。
- CONNACK4/5 最多连续拒绝五次，然后等待真实用户唤醒或重启。
  Wi-Fi 自动重连、任务到达和 WS token 自动续期不解除此限制。
- 客户端停止/销毁和 TLS 建连由 worker 执行，不在主任务或 MQTT 回调中执行。

## 接收、去重与回执

任务必须是含 task_id、kind=reminder、content、fire_at 的 JSON 对象。
task_id 最长 128 字节，字符限定字母/数字和 _ . : -；整包最长 4096 字节。
content 保留原文，fire_at 校验 UTC 格式，但不因时间已过而丢弃补派任务。
拒绝 retained、其他主题、畸形/重复键、NUL、错误分片和超长消息。

队列最多 8 条等待任务，另有 1 条交给主任务的任务。
排队/已领取任务同 ID 不重复入队。队满明确记录日志且不发已播报 ACK。
协议尚未确定时队列不会自行清空；此时平台任务测试只验证“已接收”，不能批量压入无限任务。
队列是 RAM，不保证设备断电后恢复；平台文档的补派仅适用于仍为 pending 的任务，
不能假定平台一定会重发“已投递但未完成”的任务。

已播报 ID 保存在独立 NVS namespace device_tasks / played_v1，保留最近 32 条，
记录包含版本和设备身份。重启加载；损坏/读取失败时阻止任务播报，避免静默重复。
保存失败保留 RAM 去重记录，5 秒后重试；持久化前暂停 ACK 和下一条播报。

完成流程：

1. 语音接口确认后才允许主任务从队列领取。
2. 待机时单独打开 WS，不进入 listening、不上传麦克风。
3. 复用 WS 解码与 PCM 输出；网络 audio.done 本身不算完成。
4. 实际有解码音频，收到结束边界并排空输出后，主任务归还 played=true。
5. worker 先持久化 task_id，成功后向 devices/{device_id}/ack 发送
   仅含 task_id 的 JSON，QoS1、retain=false。
6. 已播报重复任务不再播放，但再次 ACK。重连/重启时可重发最近完成记录的 ACK，
   平台按文档幂等处理；PUBACK 仅确认 broker 接收，不等价于平台数据库更新。

手动取消、连接/鉴权失败、无音频响应和超时都不记已播报、不发送完成 ACK。
失败不自行重试语音请求，防止已经播出的内容重复。
物理播放和 Flash 提交之间突然断电仍存在极短的重复窗口；当前协议无法提供
跨设备掉电和远端数据库的“严格恰好一次”保证。

## 语音协议确定后的接入位置

当前 DEVICE_MQTT_FORWARD_TASK_TO_WS=0。
预留的是之前约定的“原文转发 WS”适配器，**不是已确认的新平台协议**。
只有平台明确接受这个格式，才可在私有 device_mqtt_local_config.h 将它设为 1。
若平台需要另一种 JSON，则只修改请求适配接口，不改 task 解析、去重和 MQTT 层。

需要平台确认：

- 请求的 type、字段、task_id 如何携带；是否请求按 content 原文播报。
- 音频编码、采样率、帧格式是否继续使用当前 Opus 通道。
- 音频与任务的关联 ID、开始/结束/错误事件，以及取消规则。
- 连接已鉴权后何时允许请求；有其他回答时是否需要新会话。
- 接收成功但设备重启/队满/播放失败的任务，平台是否允许重新派发。

第一版适配器默认等待当前对话结束后播报，播完回待机。
如果要求到点强制打断对话，应另行明确优先级与用户体验，不混改现有对话行为。

## 验证与实板联调

主机运行：python scripts/check_device_mqtt.py --cc <本机TCC路径>。
同时运行 check_handshake.py、check_device_auth.py、check_audio.py。
主机测试覆盖生产解析和服务函数，模拟 MQTT/NVS/时钟，不能代替真实 TLS、
Wi-Fi 掉线时序、音频效果、Flash 写入耗时和双连接峰值内存测试。

获得 CA 后先验证连接、状态页面、SUBACK、普通任务接收、重复任务、断网恢复；
不要在公共平台故意发布别人的主题。ACL 负向测试应在授权的测试环境完成。
语音协议完成后再验证实际完整播报、取消不 ACK、重启去重和回执页面。

本次没有烧录设备，没有向 GitHub 提交或推送。

### 本轮已完成的验证

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
