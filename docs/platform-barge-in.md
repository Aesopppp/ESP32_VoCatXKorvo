# 平台判定打断（2026-11-13）

适用工程：D:\ESP32_VoCatXKorvo。按本次确认的浏览器控制台流程修改，音频继续使用现有 Opus；未切换 PCM，也未直连豆包原生协议。HTTP 握手、动态 URL/token、MQTT 和唤醒入口沿用原实现。

## 运行契约

| 事件/动作 | 设备行为 |
| --- | --- |
| 唤醒并通过鉴权 | 启动同一个麦克风上传任务 |
| 监听、播放、平台打断 | 持续上传 AEC 后真实音频；不因本地 VAD 门控、静音或填零 |
| input_audio_buffer.speech_started | 立即停本地播放、丢弃旧 Opus/PCM 队列；上传不中断 |
| input_audio_buffer.speech_stopped | 开启新回复窗口，等待回答音频；上传不中断 |
| response.audio.done | 关闭本轮接收窗口，播完已接收尾音后回监听；迟到二进制不能重新开窗 |
| response.done status=cancelled / response.cancelled | 当前回复被平台终止，停播清队列；不发 cancel、不等待 ack、不因此重连 |
| 普通未知事件（含未展示的实时字幕） | 忽略，不断连接 |
| error | 按现有鉴权/业务错误分流；不当作音频完成边界 |

设备不再发送 response.cancel，也不发送 input_audio_buffer.commit 或 response.create 来代替平台断句。服务端负责结束旧模型输出，supports_overlapping_speech=False 属于服务端设置。待机仍停止上传并断开语音连接；真实网络、鉴权、输入失败及回复超时仍保留恢复处理。

上行：16 kHz、单声道、20 ms/包，input_audio_buffer.append 的 audio 字段为单个 Opus 包 Base64，encoding=opus。下行：现有 raw Opus 二进制消息；一条消息一个完整包。没有增加 PCM/Ogg/MP3 自动探测或新的 audio_format 协商。

## 实现位置

- main/app_main.c：不再执行本地人声打断或暂停/停止上传来交接回答；平台事件只更新界面和计时。
- main/wake_word.c：保留 WakeNet、AEC 和连续 PCM 生产；删除本地打断检测、1.5 秒预录音保护与取消后补传。短 FIFO 上限 512 ms，网络积压时保留较新音频并限频报告丢帧。
- main/audio_in.c：同一任务/编码器跨越监听与播放；使用绝对 20 ms 节拍补偿短调度抖动，补偿期间两包至少间隔 10 ms，超过 100 ms 的阻塞重新建立时基。
- main/ws_service.c：网络回调只更新有界协议状态和原子标志。worker 清播放、跳过旧代次音频；连接级鉴权/error 不随音频代次丢弃。response ID 用于拒绝最近旧响应的迟到结束事件。
- main/audio_out.c：原子停止请求可解除 PCM 背压；worker 清队列、刷新 DMA，保留麦克风共享 I2S 时钟。
- main/response_stream.c：平台打断直接关闭旧输出；audio.done 后不因迟到二进制/普通工具事件重新开窗。
- sdkconfig 和 sdkconfig.defaults：启用 CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK，避免 RX 长读取占用同一把发送锁。真实写失败不盲目重发，避免重复音频。

无 ID 的二进制音频无法判断服务器是否在新窗口中混入旧回答；平台必须按同一 WebSocket 有序发送，在新回复开始后不再混发旧音频。可选 response.created/response_id 应保持一致。正常回复一轮只有一个最终 audio.done；工具调用后如需新响应，发送明确的新 response.created 或进入下一次 speech_stopped。

## 操作变化与验证

- 待机触屏唤醒、监听时触屏结束、BOOT 长按 3 秒配网、菜单音量/亮度继续保留。
- 播放时轻触不再主动取消；本次未增加物理硬停 cancel 接口。
- scripts/check_audio.py 编译生产解析器、回复状态机、上传任务、主任务交接和 WS 分发函数；模拟依赖不代替双核/网络/声学验证。
- HTTP/鉴权/MQTT 分别使用 check_handshake.py、check_device_auth.py、check_device_mqtt.py 回归。
- 实板需验证：正常问答、长回答中说短句、多次连续打断、停顿后继续讲话、60 秒待机及再次唤醒；平台日志确认没有 response.cancel，设备日志应出现 Platform interruption 和 microphone continues。
- 本轮只编译，不烧录；主机测试通过不能证明真实环境无丢包、回声或网络延迟。

修改前完整备份：D:\ESP32_VoCatXKorvo_backups\before-platform-barge-in-20261113-095953\ESP32_VoCatXKorvo。构建与验证日志保存在 work/platform-barge-in-20261113 和各检查脚本的 work 子目录。

## 连续收发的播放卡顿修正（2026-11-13）

实板日志曾出现播放缺数据累计 16 次、解码墙钟耗时最高 138 ms、麦克风 FIFO 持续溢出。此次修正保留持续 Opus 上行、平台打断及原有结束边界：

- 编码上传从 core 1/priority 5 调整到 core 0/priority 4，低于 AFE feed 的 priority 5。解码保留在 core 1，并从 priority 4 调整到 5；PCM 输出仍为 core 1/priority 6。未新增任务。
- `audio_upload_policy.h` 集中管理发送时基。旧版超过 2 ms 延迟就重新计时，会使多次短延迟累积成录音积压；新版保留绝对节拍，有限追赶，长期阻塞重新计时。20 ms/包、16 kHz 单声道、Opus 码率及连续真实音频不变。
- PCM 缓冲仍为 500 ms，起播预填仍为 150 ms；不通过扩大缓冲掩盖处理速度不足。
- 新增诊断字段使 RX 队列每项增加约 12 字节，4096 项约增加 48 KiB PSRAM；没有新增大块内部 RAM 缓冲。

诊断日志默认每 5 秒汇总，不打印音频/正文/凭据：

| 日志 | 解释 |
| --- | --- |
| `Upload stats` | 本窗口成功上传包数、音频时长、FIFO 剩余时长，以及编码、同步发送和节拍迟到的最大耗时 |
| `Playback stats` | 原有缺数据统计；新增本窗口解码总耗时、解出的音频时长、解码耗时超过本包音频时长的次数。耗时包含任务抢占 |
| `RX audio stats` | `received` / `after_eof` 是本连接累计包数；`gap_max_ms` 是本连接同窗口内的最大收包间隔；`queue_wait_max_ms` 是本统计窗口中最大的消息队列等待，正常 PCM 背压也会产生等待 |
| `Audio boundary` | 最终结束事件到达时的音频包序号，以及该事件等待 worker 的时长；可与后续包序号核对 |
| `Audio arrived after final audio.done` | 入队前就已处于 EOF，说明音频在接收顺序上晚于结束事件；并非 worker 解码慢导致的误判。仍拒绝迟到音频，不因此重连 |
| `Microphone FIFO behind` | 汇总上次报告以来被丢弃的帧数/时长，避免将限频日志误读成只丢了一帧 |

若收包间隔小、队列积压大且解码耗时高，应继续检查设备实时性能；若队列空且收包间隔大，应检查网络/平台供音。长回复被平台快速推送时，队列等待大本身不表示故障，需结合 `starvations` 判断。

EOF 后音频不会自动接纳：无 ID 的 raw Opus 无法可靠区分迟到旧音频与新回答。平台仍应按“本轮全部音频 → 最终 response.audio.done”发送。此次只增加可验证的证据，不推测新的分段协议。

本次源码快照、构建脚本及结果：`work/playback-fix-20261113-174252/`。主机回归覆盖调度迟到、长期阻塞、连续分包、暂停恢复、发送失败、平台打断和 EOF 前后入队次序；不代替实板 CPU/网络/声学测试。实板建议连续播放长回答，再测试讲话打断、多轮回答和待机后唤醒，观察是否仍出现播放缺数据与 FIFO 溢出。
