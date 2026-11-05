# YMOS → ESP-VoCat v1.2 无屏移植

## 版本与范围

- 工作目录：`D:\ESP32_VoCatXKorvo`（按用户要求将乘号 `×` 改为英文字母 `X`）。
- 基线：`D:\ESP32-ymos-1_backups\before-full-duplex-20261029-145338\ESP32-ymos-1`。
- 基线提交：`91d1c9aa320349d2d512dd9e7ba90409091386a8`。
- 公开仓库：`https://github.com/Aesopppp/ESP32_VoCatXKorvo`，默认分支 `main`；本地保留 `port/vocat-v1.2` 基线历史。原工程和备份未修改。
- SDK：ESP-IDF 6.1，ESP32-S3、16 MB Flash，Octal PSRAM 自动识别（本次实板日志为 16 MB）。
- 硬件参考：已运行过的 XChatbot VoCat v1.2 板级代码与 `BoxAudioCodec`；未引入其 C++ 应用框架。

本版保留 YMOS 的 C 服务结构、事件总线、WebSocket 鉴权与语音协议、连续对话、打断、句首预录音、NVS 和本地网页配网。按最新要求，**不启动 MQTT、不接入远程定时提醒**，也未合入另一个 YMOS 工作目录中新加的设备 HTTP 握手。

不初始化屏幕、触摸、动画、SD 卡、IMU 或电量计。屏幕背光关闭，没有图标和菜单。`led_service` 只保留逻辑状态接口；串口日志是本版观察状态的方式。旧 Korvo 的红/绿灯和 ADC 按键不适用于此板。

## 引脚与硬件初始化

| 功能 | VoCat v1.2 设置 |
| --- | --- |
| 音频电源 | GPIO48，高电平使能；冷启动和软复位均保持供电 |
| 板级 POWER_CTRL | GPIO9，保持已运行 VoCat 代码的低电平 |
| 功放 PA | GPIO15，由 ES8311 驱动管理 |
| I2C0 SDA / SCL | GPIO2 / GPIO1，400 kHz |
| ES8311 / ES7210 地址 | 7-bit `0x18` / `0x40`；codec_dev 接口使用左移后的 8-bit 地址 |
| I2S0 MCLK / BCLK / WS | GPIO42 / GPIO40 / GPIO39 |
| I2S0 DOUT / DIN | GPIO41 / GPIO3 |
| BOOT | GPIO0，低电平按下 |
| LCD 背光 | GPIO44，保持低电平 |
| 日志控制台 | 原生 USB Serial/JTAG，避免 UART0 占用 GPIO43/44 |

`board_service_init()` 负责供电和 I2C 总线；上电稳定后有界重试探测两颗 Codec。`audio_out` 借用这条总线，不负责删除它。该初始化固定面向 **v1.2**，不自动探测并兼容其他 PCB 版本。

## 音频链路

```text
ES7210 四槽 TDM → codec_dev 选取 slot0/slot1 → 16 kHz MR
  → AFE(AEC/VAD/WakeNet 或 LITE) → PCM 历史缓冲 → Opus → ACOS WebSocket

ACOS WebSocket → Opus 解码 → YMOS PCM 队列 → codec_dev → ES8311 → 功放/扬声器
                                                        ↓ 硬件回采
                                                    ES7210 MIC3
```

- VoCat ES7210 使能 MIC1–MIC4，TDM 槽序按参考实现为 MIC1、MIC3、MIC2、MIC4；取前两槽作为 `M`（麦克风）和 `R`（扬声器回采）。增益接口使用物理 MIC 编号，不能把槽号直接当作增益通道号。
- 两路输入同步采样为 16 kHz / signed 16-bit，AFE 输入由 Korvo 的 `MMR` 改为 `MR`。不再额外拼接软件参考，也不保留原软件参考环形缓冲。
- 输出单声道 16 kHz，经 codec_dev 适配共享时钟的槽宽。输入/输出打开后持续运行；空闲播放零数据，取消回答时只清播放队列/DMA 尾音，不停麦克风时钟。
- 保留原播放预填充 150 ms、PCM 队列 500 ms、DMA 6 × 10 ms，以及 YMOS 打断判定和约 1.5 秒句首预录音策略。
- 麦克风和回采增益初始均为 30 dB，沿用 VoCat 已有驱动默认值；并不意味着所有音量和外壳条件都已完成声学调试。
- 每五秒的 `AFE input MR` 日志汇总两路峰值和接近满幅的采样数。未播放时参考应较低；播放时应有参考信号。峰值/削顶只能辅助排查，不能代替听感和回声验证。
- WAKE/LITE 的帧长、输入通道数必须一致，否则初始化失败，避免错误长度造成访问越界。

## 操作方法

1. 正常上电，等待日志 `BOOT ready` 和 `Wi-Fi ready`。首次无可用 Wi-Fi 时自动进入配网。
2. 需要重新配网时，运行中按住 **BOOT 约 3 秒**。每次按住只发送一次配网事件，短按没有功能；配网期间再次长按也不会退出。
3. 手机连接 `YMOS-XXXX` 热点，密码 **`ymos1234`**。热点后缀沿用原程序，每次启动配网可能不同。
4. 若系统未自动弹出页面，在浏览器打开 **http://192.168.4.1/**。扫描并选择家中 Wi-Fi、填写密码、提交。页面由设备本地提供。
5. 提交后尝试连接；连接失败按原逻辑回滚并重新配网。无操作配网窗口为 10 分钟；到期尝试恢复已保存网络。
6. 正常联网后说 **“你好小智”** 唤醒（模型 `wn9s_nihaoxiaozhi`），再开始提问。此备份基线在唤醒时建立 WebSocket；进入待机则断开。不是 XChatbot 的待机常连策略。
7. 对话期间可继续讲话打断，连续静默约 60 秒回到等待唤醒。默认音量 40；已有 YMOS NVS 音量优先。本版未提供音量键或触屏调整。

**BOOT 的两个时机不同：**上述长按针对应用已经运行的状态。在上电/复位前按住 BOOT 会进入芯片下载模式，应用不会运行。初始化时若按键一直按住，策略要求先松手，防止启动误触。

## 模块边界与后续开发

| 文件 | 职责及本轮变化 |
| --- | --- |
| `main/board_config.h` | 集中 VoCat 引脚、通道和增益，以及 ACOS 默认地址 |
| `main/board_service.*` | 新增 C 板级供电/I2C 初始化 |
| `main/audio_out.*` | I2S TX + TDM RX、硬件回采，原播放队列/取消规则保留 |
| `main/wake_word.c` | MR 输入、帧长检查、诊断；VAD/预录音/打断策略保留 |
| `main/button_service.c` | GPIO0 轮询，向事件总线发布配网请求 |
| `main/boot_button_policy.h` | 将 BOOT 映射到现有 YMOS 长按策略，可独立回归 |
| `main/app_main.c` | 仍为业务事件唯一消费方；移除 MQTT/提醒调度；BOOT 只进入配网 |
| `main/led_service.*` | 保持旧状态调用接口，去除 TCA9554 及供电副作用 |
| `main/provisioning_service.c` | 复用本地 SoftAP/HTTP/DNS 配网，无屏也能使用 |
| `main/cfg_service.*` | 沿用 NVS 数据布局，不删已有 MQTT 配置键 |

为便于后续与 YMOS 对比，原 `mqtt_service.*`、`mqtt_topics.*`、`acos_reminder_service.*`、`reminder_message.*` 和 `tca9554.*` 源码仍留作参考，但不列入本版编译清单。依赖清单移除了 MQTT。WebSocket 内保留基线的提醒兼容接口，目前无应用入口调用；它不会自行订阅或请求提醒。历史 MQTT 文档不代表本固件已启用这些功能。

下一阶段接触屏时，通过事件总线进入配网/音量/对话业务，不在触摸回调里直接停止音频、操作网络或写 NVS。首先独立实现显示/触摸服务，再由主任务转发状态，避免把 LVGL 混入实时音频路径。

## 构建与验证边界

工程目录已按用户要求改为纯英文路径 `D:\ESP32_VoCatXKorvo`，解决 Windows Xtensa GCC 对乘号 `×` 的兼容问题。源码和构建输出都在同一个工程目录中，不需要英文别名、构建副本或额外同步脚本。

使用工程匹配的 ESP-IDF 6.1 环境：

```powershell
cd D:\ESP32_VoCatXKorvo
idf.py build
```

VS Code 可以直接打开该目录或 `VoCat.code-workspace`，使用 ESP-IDF 扩展的编译按钮。本机 `.vscode/settings.json` 指向已安装的 6.1 工具链，未固定 COM 口；烧录前选择实际 VoCat 端口。

最终产物为 `build/vocat_korvo_agent.bin`，同目录包含 bootloader、分区表、OTA 初始数据和唤醒模型等产物。`sdkconfig.defaults` 从本次实际配置导出。依赖保持 YMOS 基线版本，仅移除 MQTT。

之后自行烧录时，在上述工程目录、IDF 6.1 终端中，使用实际确认的 VoCat COM 端口执行 `idf.py -p COM端口 flash monitor`。本次未执行此命令。

测试包含原预录音/打断策略、响应生命周期测试，以及 BOOT 的消抖、3 秒边界、一次长按一次事件、松开重置、启动按住、计时回绕。构建和逻辑测试无法证明真实回采、扬声器、按键电气或网络声学效果。

### 2026-11-04 本地验证结果

- ESP-IDF 6.1 完整编译、链接和分区容量检查通过，日志在 `work/vocat-port/build-result.log`。
- 应用镜像为 2,009,440 字节，3 MiB 应用分区剩余 1,136,288 字节（约 36%）；唤醒模型镜像为 125,951 字节。
- 预录音、打断保护、原按键策略、BOOT 策略交叉编译回归通过；响应流测试使用生产 JSON 解析器及生命周期代码，369 项检查通过。
- 实际构建清单不包含 MQTT、TCA9554 或 LVGL。依赖版本保持基线；12 个复用模块/分区文件经换行归一化后与备份一致，网络默认配置亦未改变。
- 此次 Windows 构建曾在 Ninja 的长 `cmd.exe` 包装命令处挂起。相同参数的 IDF `ldgen.py` 直接执行正常；最终构建仅在本机生成的 `build/build.ninja` 中采用等价的直接启动器 `work/vocat-port/run-ldgen.py`，未修改 SDK 或固件源码。原生成文件保存在 `work/vocat-port/build.ninja.before-ldgen-wrapper`。这些均为忽略的本地构建资料，重新配置 CMake 会恢复标准生成命令。
- 未烧录或进行实板验证；不能据此认定 AEC、GPIO 电气、真实音量和网络声学表现已通过。

本轮不烧录。首次部署用本工程生成的完整 flash 清单，**不能只把应用 `.bin` 当作 XChatbot 的 OTA 包**；二者分区布局不同。不要手工猜偏移或执行全片擦除。烧录前核对 VoCat 端口，保留需要的原固件/持久数据。设备程序尚未更换时，屏幕仍显示旧固件属于正常情况。

实板首轮应验证：冷启动与软件复位 Codec 均应答、BOOT 长按配网、保存后重启自动联网、唤醒/多轮问答、播放时保持安静不误打断、短句打断能保留句首、60 秒待机后再次唤醒。出现问题时同时保留启动日志、`AFE input MR`、播放统计和回答期间日志。

## ACOS 连接配置修复（2026-11-04）

首轮实板日志已确认 Wi-Fi、唤醒和 WebSocket 传输建连成功，但随后出现 `Configure ACOS token in NVS`。基线默认 token 为空，导致本地鉴权帧尚未发出就进入恢复流程。

- 当前目标：`wss://119.29.215.114/v1/realtime`。地址集中在 `main/board_config.h`。
- 本机测试密钥放在 `main/acos_local_config.h`，已加入 `.gitignore`；可共享模板为 `main/acos_local_config.h.example`。从 Git 获取工程后需复制模板并填写有效密钥。
- `cfg_service_init()` 在网络服务初始化前，以预设编号一次性更新 `ws_url` 和 `acos_token`，因此已有空 token 或旧地址也能迁移。两项配置先提交，再单独提交完成标记；失败则返回错误，下次启动重试。未执行全片擦除，不更新音量、Wi-Fi 或设备身份。
- 相同编号不会在每次开机时反复覆盖用户配置。日后修改地址/密钥时需递增 `LOCAL_ACOS_PRESET_ID`，重新编译烧录。
- TLS 证书验证、鉴权消息格式、音频和对话状态机均保留原实现。本轮未操作设备或发起真实语音会话。

重新烧录后，第一次启动应出现 `Local ACOS preset applied once`。联网后说“你好小智”，鉴权成功应出现 `WebSocket authenticated; audio upload ready`，随后进入监听。前者只表示配置已保存；后者才表示平台鉴权通过。

主机模拟覆盖生产预设函数的旧/空凭据迁移、后续用户配置保留，以及六个 NVS 读写/提交失败与重试场景；这不等同于实板断电测试。验证资料位于 `work/acos-connection/`。
