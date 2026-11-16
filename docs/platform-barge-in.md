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
- main/audio_in.c：同一任务/编码器跨越监听与播放；AFE 的真实采样时钟驱动上传，每 320 个采样组成一包 20 ms Opus。默认不申请整句录音缓存，不额外等待发送节拍。
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

## 连续收发的播放卡顿修正（2026-11-13，历史记录）

以下记录当时的实现。发送时基和预填参数已由文末 2026-11-16 调整取代。

实板日志曾出现播放缺数据累计 16 次、解码墙钟耗时最高 138 ms、麦克风 FIFO 持续溢出。此次修正保留持续 Opus 上行、平台打断及原有结束边界：

- 编码上传从 core 1/priority 5 调整到 core 0/priority 4，低于 AFE feed 的 priority 5。解码保留在 core 1，并从 priority 4 调整到 5；PCM 输出仍为 core 1/priority 6。未新增任务。
- `audio_upload_policy.h` 集中管理发送时基。旧版超过 2 ms 延迟就重新计时，会使多次短延迟累积成录音积压；新版保留绝对节拍，有限追赶，长期阻塞重新计时。20 ms/包、16 kHz 单声道、Opus 码率及连续真实音频不变。
- PCM 缓冲仍为 500 ms，起播预填仍为 150 ms；不通过扩大缓冲掩盖处理速度不足。
- 新增诊断字段使 RX 队列每项增加约 12 字节，4096 项约增加 48 KiB PSRAM；没有新增大块内部 RAM 缓冲。

诊断日志默认每 5 秒汇总，不打印音频/正文/凭据：

| 日志 | 解释 |
| --- | --- |
| `Upload stats` | 本窗口成功上传包数、音频时长、FIFO 剩余时长，以及编码和同步发送的最大耗时；11-16 起删除额外发送时钟及其 late_max_us 字段 |
| `Playback stats` | 原有缺数据统计；新增本窗口解码总耗时、解出的音频时长、解码耗时超过本包音频时长的次数。耗时包含任务抢占 |
| `RX audio stats` | `received` / `after_eof` 是本连接累计包数；`gap_max_ms` 是本连接同窗口内的最大收包间隔；`queue_wait_max_ms` 是本统计窗口中最大的消息队列等待，正常 PCM 背压也会产生等待 |
| `Audio boundary` | 最终结束事件到达时的音频包序号，以及该事件等待 worker 的时长；可与后续包序号核对 |
| `Audio arrived after final audio.done` | 入队前就已处于 EOF，说明音频在接收顺序上晚于结束事件；并非 worker 解码慢导致的误判。仍拒绝迟到音频，不因此重连 |
| `Microphone FIFO behind` | 汇总上次报告以来被丢弃的帧数/时长，避免将限频日志误读成只丢了一帧 |

若收包间隔小、队列积压大且解码耗时高，应继续检查设备实时性能；若队列空且收包间隔大，应检查网络/平台供音。长回复被平台快速推送时，队列等待大本身不表示故障，需结合 `starvations` 判断。

EOF 后音频不会自动接纳：无 ID 的 raw Opus 无法可靠区分迟到旧音频与新回答。平台仍应按“本轮全部音频 → 最终 response.audio.done”发送。此次只增加可验证的证据，不推测新的分段协议。

本次源码快照、构建脚本及结果：`work/playback-fix-20261113-174252/`。主机回归覆盖调度迟到、长期阻塞、连续分包、暂停恢复、发送失败、平台打断和 EOF 前后入队次序；不代替实板 CPU/网络/声学测试。实板建议连续播放长回答，再测试讲话打断、多轮回答和待机后唤醒，观察是否仍出现播放缺数据与 FIFO 溢出。

## 音频优先的设备侧精简（2026-11-16）

后续日志中，解码最大墙钟耗时已降到约 12 ms，但上传仍有积压丢帧，发送最大耗时达 135 ms，接收间隔达 231 ms；同时观察到播放期间表情卡顿。接收间隔由设备回调观测，不能单独证明是平台或网络问题。本轮先减少已确认的设备热路径开销：

- **上传由采样驱动**：AFE 每 32 ms 提供 512 个样本，按块拷贝拼成 320 样本的 Opus 包；不逐样本查询暂停状态，也不再等待第二套 20 ms 发送时钟。一次读取可能产生一包或两包，平均音频时长仍由 16 kHz 采样决定。短暂阻塞后按顺序消费已有音频；每个 AFE 帧主动让出一个 RTOS tick，空 FIFO 短等待，原有 512 ms 有界 FIFO 和过载丢帧日志保留。持续吞吐不足仍需真机定位。
- **移除默认整句缓存**：当前应用未注册整句回调，不再预分配 640,000 字节或复制整句 PCM。轻量能量 VAD 和活动事件继续用于空闲检测。可选整句 API 仍保留，仅在采集完全停止后注册时申请 PSRAM；注销释放。回调只借用缓存到返回，不得阻塞上传。均方能量累加改为 64 位，避免大振幅溢出。
- **固定上传缓冲**：单个 Opus 包最大 256 字节，与编码输出上限一致。Base64/JSON 使用约 946 字节静态缓冲，不再每包 malloc/free。正常仅 audio_in 调用；并发误用被立即拒绝。网络错误不自动重发音频。
- **300 ms 起播储备**：PCM 总容量保持 500 ms，起播预填由 150 ms 调到 300 ms，给短暂供数间隔留余量。相较原版，首音可能多等待约 150 ms；不足 300 ms 的短回答收到 EOF 后立即允许起播。平台打断仍立即请求停播/清旧队列，不等待缓冲播完。
- **对话静态图**：默认仅在监听/回答/等待等状态切换时刷新首页状态图；待机 20fps 和菜单滑动保留。`main/ui_face.h` 的 `UI_FACE_ANIMATE_DURING_CONVERSATION` 默认 0，可编译为 1 对比动画负担。未修改 LCD DMA 传输、板级引脚和音频通道。

Opus 格式、HTTP 握手、MQTT、平台鉴权、连续上行和平台打断契约不变。尤其没有恢复设备 VAD 自行打断或 response.cancel，也没有放开 audio.done 后的无 ID 音频接收。平台最终结束事件仍需在最后一个音频包之后发送。

验证入口：`scripts/check_audio.py` 检查真实采样节奏、135 ms 发送阻塞后追赶、逐样本顺序、半包暂停恢复、发送失败、可选缓存和活动事件、固定上传缓冲、300 ms 预填/短 EOF、平台打断及回复结束边界。`scripts/check_ui.py` 检查真实 LVGL 状态画面、对话静态时没有重复刷屏、菜单和触屏操作。主机检查不证明真实多核、声学或网络性能。构建/证据目录：`work/audio-smooth-20261116/`。

实板优先验收：连续长回答、回答中插话、多轮停顿对话、60 秒待机再唤醒；观察 `Playback stats` 的 `starvations` 增长、`Upload stats` 中音频时长与窗口时长的差异，以及 `Microphone FIFO behind` 是否持续出现。播放期间表情静止是本版主动减负的行为；若 `Audio arrived after final audio.done` 继续增加，仍需平台核对发送顺序。

## 播放期间上行积压优化（2026-11-16，后续测试）

新日志显示：播放期间多个约 5 秒窗口只上传 4.28～4.50 秒音频，FIFO 长期接近 512 ms，并持续丢弃旧帧；同时 AFE 无超预算、解码未出现慢包，播放缺数累计仅一次。这证明上行消费速度不足，但最大墙钟耗时不足以区分编码运算、调度抢占和网络等待的比例。警告不能直接归因于平台，也不能通过扩大 FIFO 或隐藏日志解决。

本轮保留音频任务绑定、AEC、队列容量和平台协议，只做以下调整：

1. `main/board_config.h`：`OPUS_COMPLEXITY` 从 2 降到 0。减少编码计算，采样率、声道、20 ms 帧长和 24 kbps 码率不变。可能影响上行音质，实际识别和远场表现需实板对比；没有关闭 AEC 或把麦克风静音。
2. `main/wifi_service.c/.h`：新增音频性能模式接口。`audio_in` 开始连续上传时申请 `WIFI_PS_NONE`，完全停止或创建任务失败时恢复默认 `WIFI_PS_MIN_MODEM`；监听/播放/平台打断不切换模式。关联 AP 后重应用当前意图，配网/未关联时不调用省电切换；切换失败记录日志，不以此触发重连。连续对话期间功耗可能增加，待机恢复原模式。
3. `main/audio_in.c`：每 5 秒增加 `encode_work_ms`、`send_work_ms`，分别汇总编码和同步发送的墙钟耗时，包含抢占/锁/网络等待，不能当作纯 CPU 用量。与 `window_ms`、`audio_ms`、`fifo_ms` 一起判断下一步瓶颈，避免只看一次最大耗时就调整任务优先级。

回归使用生产 Wi-Fi 事件处理器、上传生命周期和分包逻辑，覆盖：重连恢复性能模式、停止后恢复省电、切换失败仍可上传、任务创建失败和停止超时，以及既有平台打断/播放排空/EOF 边界。主机桩函数不证明真实调度或吞吐改善。构建与修改前快照保存在 `work/upload-duplex-20261116/`。

烧录后的验收：

- 唤醒后出现 `Wi-Fi power save: NONE (continuous audio)`、`complexity=0`；结束对话后出现 `MIN_MODEM (idle)`。
- 连续播放 30～60 秒回答并在中途说短句，检查多个窗口的 `audio_ms` 是否总体跟上 `window_ms`，`fifo_ms` 是否不再长期顶在 480～512 ms，丢帧警告是否明显减少。单个窗口可因补发积压而超过 5000 ms，不能只比较一行。
- 重复平台打断、60 秒待机再唤醒、Wi-Fi 断线恢复，检查语音识别、尾音和播放缺数。若仍积压，依据新的累计耗时再评估是否需要拆分编码与发送；本轮没有新增音频任务或额外队列。
