# VoCat 设备 HTTP 握手与语音鉴权

## 1. 目标和本轮范围

工程：`D:\ESP32_VoCatXKorvo`，ESP-VoCat v1.2，ESP32-S3，ESP-IDF 6.1。
移植日期：2026-11-09。复用 `D:\ESP32-ymos-1` 已实现的纯 C 握手模块，
沿用当前工程的服务接口、主任务轮询和独立后台网络任务结构。
当前依据《DEVICE-MQTT-INTEGRATION.md》v1.4（2026-11-09）的 **HTTP 握手和语音凭据**部分，
沿用 XChatbot 工程已经确认的「路径 A：平台预登记、签发设备 ID 和 PSK」：

```text
平台登记设备 → 平台签发 device_id 和原始 PSK → 写入设备配置
    → Wi-Fi 获取 IP → HTTP POST /api/device/v1/handshake
    → 校验完整响应 → RAM 保存 MQTT/WS 运行配置，按 server_time 校时
    → 唤醒时检查 token 剩余有效期 → WS 复制下发的 URL/token → 鉴权 → 开始监听
```

主要工作是配置校验、HTTP 生命周期、动态语音凭据和失败恢复。没有修改板级引脚、音频采集、
唤醒词、打断算法、60 秒待机或触屏/按键策略；由应用主任务串行协调连接入口。

**本文记录 HTTP/动态 WebSocket 凭据阶段；2026-11-10 后续新增的 MQTT/status/task/ACK 见
[设备 MQTT 接入](device-mqtt.md)。当前仍不下载固件。**
当握手配置启用时，语音服务必须等有效的 HTTP 结果，不能因 HTTP 错误、缺少配置或 token
失效而回退到旧 ACOS key。仅配置明确 `enabled:false` 时使用旧 NVS 语音配置。
鉴权仍发送 `{"type":"auth","token":"..."}`，成功事件仍为 `__proxy_connected__`；
平台文档没有展开 WS 报文，保留现有格式作为本轮联调假设，不擅自改成 Bearer 请求头。

仓库默认配置**关闭**，不请求示例域名；本机私有配置已填写用户提供的平台注册设备 ID 和 PSK。
按用户要求暂用原 WSS 的同一台服务器，将 HTTP 基础地址设为
`https://119.29.215.114`，请求路径为 `/api/device/v1/handshake`。
本地址与新版协议的 443 握手入口一致。早期电脑探测曾返回 HTML 404，后续用户实机日志
已出现 `HTTP handshake ready`，因此不能再将早期 404 当作当前平台状态。
先前语音报 API Key 无效时，程序还在发送旧 NVS key；本轮改为使用握手返回的 token。
只有 Wi-Fi 获取 IP 且未处于配网模式时才执行握手；单纯结束对话、语音唤醒或
WebSocket 重连在有效期充足时复用快照，接近过期或鉴权拒绝才触发受控续期。
触摸判定及待机关闭 WebSocket 的行为保持原样。
本轮没有烧录、改分区、改 SDK、写 eFuse 或推送 GitHub。
握手收到 401/403/404 等永久错误后停止自动重试，语音也不建立；修复配置后重启，
或由后续配置入口显式调用 refresh。普通 Wi-Fi 重连与自动 token 续期不解除永久错误。

## 2. 平台需要提供的参数

| 参数 | 说明 |
| --- | --- |
| `server_url` | device-server 的 HTTP/HTTPS 基础地址；与语音 WS/WSS 地址不同 |
| `device_id` | 平台登记时签发，逐字使用，大小写敏感 |
| `psk` | 平台签发的原始 PSK；不是哈希、原 ACOS token 或 MQTT password |
| `device_model` | 默认 `esp32s3`；需要与平台的型号约定一致 |
| HTTPS 证书 | 使用系统证书包验证服务端；私有 CA 需单独配置和验证 |

设备号支持协议约定的 `[A-Za-z0-9_.:-]`、1～64 字符，同时兼容文档测试号 `dev-001`
和正式号 `dev_7k2m9x4p0q3z`。固件不生成平台 ID，不用 MAC 替代，不自动登记租户。
原有 `korvo_cfg/device_id` 是 YMOS 的旧身份，与本模块隔离。

## 3. 开发期如何启用

在工程根目录执行一次（已有私有文件时直接编辑，不覆盖）：

```powershell
Copy-Item -LiteralPath main\device_handshake_local_config.example.h -Destination main\device_handshake_local_config.h
```

修改私有头文件中的 `DEVICE_HANDSHAKE_LOCAL_JSON`：

```json
{
  "enabled": true,
  "server_url": "https://device-server.example.com",
  "device_id": "REPLACE-WITH-REGISTERED-ID",
  "device_model": "esp32s3",
  "psk": "REPLACE-WITH-DEVICE-PSK",
  "allow_insecure_http": false
}
```

上面的 JSON 用于解释内容；头文件使用 C 字符串，因此保留示例里的引号转义格式。
`server_url` 不追加 `/api/device/v1/handshake`，程序会补上；末尾 `/` 自动移除。
HTTP 测试环境必须显式设置 `allow_insecure_http: true`。
可在同一私有头文件中定义 `DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS`，默认 `300u`。
含义为新建 WS 前的安全窗口（秒）；平台新签发 token 的有效期必须大于该窗口及请求耗时。
URL 支持 ASCII 域名、IPv4、端口和路径；不支持 IPv6 字面量、URL 用户名密码和 fragment；
基础地址不支持 query。配置的未知键会报错，避免拼写错误后悄悄使用默认值。

私有头文件已加入 `.gitignore`。修改后在此工程对应的 ESP-IDF 6.1 终端运行：

```powershell
idf.py build
```

本工程的 VS Code ESP-IDF 配置也可以编译。烧录必须使用本次 build 生成的配置，
不要猜测偏移；普通应用烧录通常保留 NVS，全片擦除会丢失设备配置。

## 4. NVS 身份配置与优先级

读取顺序如下，整条配置作为一组处理，不能将不同来源的 ID 和 PSK 混搭：

1. 默认 NVS 分区，namespace **`device_boot`**，字符串 key **`config`**，内容为上述 JSON。
2. 仅当 namespace/key 不存在时，使用私有头文件。
3. 私有头文件不存在时使用 `{"enabled":false}`。

NVS 中的 `enabled:false` 也会覆盖编译配置。NVS 读取失败、类型错误、超长、隐藏 NUL 或
JSON 无效时停止新握手，不回退到可能属于另一台设备的身份。JSON 上限 4096 字节。

本轮仅增加 NVS 读取，未提供批量注入工具、配网页写 PSK 功能，也不会自动写入或擦除身份。
量产应采用每台设备独立身份、统一固件和经验证的安全注入流程；普通 NVS、私有头文件和
编译进固件的 PSK 都不等于已完成加密保护。原工程的 `cfg_service_init()` 还保留 NVS
异常时擦除分区的既有恢复代码；量产前应单独评估该行为对出厂身份的保护。

MQTT password 和 WS token 只保存在 RAM，不反复写 Flash。断网/重新配网/显式刷新会清除
可用快照。平台轮换运行凭据后，可由未来控制入口调用 `device_handshake_service_refresh()`
重新读取出厂配置并发起握手；当前未绑定按键或远程命令，重启也会重新读取配置。

## 5. 请求与响应

请求为 POST，带以下 Header：

```text
device-id: <平台签发的ID>
device-model: <配置型号，默认esp32s3>
device-version: <esp_app_get_description()->version>
authorization: Bearer <原始PSK>
content-type: application/json
accept: application/json
connection: close
```

Body 为 `{"mac_address":"<STA MAC>"}`，省略文档允许省略的 IP。
MAC 只作平台核对硬件的附加信息，不代替 PSK。

200 响应必须完整包含：

- 合法 UTC `server_time`：`YYYY-MM-DDTHH:MM:SS[.1～6位小数]Z`，转成 Unix 毫秒校时；
  不重复加北京时间偏移。接入前系统/显示时区仍由原工程决定。
- `mqtt` 六项：endpoint、client_id、username、password、publish_topic、subscribe_topic。
- `websocket.url`、`websocket.token`、`websocket.expires_in`。
  `expires_in` 必须为 1～4294967295 的整数秒；缺失、字符串、小数、零和溢出均拒绝。
- `firmware` 为 null 或包含 version/url 的对象；本轮仅保存，无 OTA 动作。

校验 username 等于本设备 ID，两个 topic 属于本设备且符合文档路径，拒绝通配符/其他设备主题。
保存的仍是平台返回的原始值，不生成密码、client_id 或 topic。响应允许新增未知字段；
重复 key、控制字符、隐藏 NUL、超过 8 层嵌套、超长和尾随垃圾会被拒绝。

当前字段上限（字节）：server_url 512、PSK 512、device_id/model 64、MQTT endpoint/password 512、
client_id 256、topic 128、WS URL 1024、WS token 2048、firmware version 64/URL 1024。
超限显式失败，不截断凭据；平台若需要更长字段，应同步更新限制和测试。

## 6. 生命周期、超时和错误恢复

| 情况 | 行为 |
| --- | --- |
| 200 且全部字段有效 | 保存 RAM 快照，停止自动请求 |
| 新签发 token 的剩余时间已不足安全窗口 | 停止并报 `websocket_token_lifetime_too_short`，防止 200 响应引发续期风暴 |
| 401 / 403 / 404 | 停止自动重试；Wi-Fi 抖动不解除；修复配置后显式 refresh 或重启 |
| 其他不可重试 4xx / HTTP 3xx | 停止，不跟随重定向，不转交 PSK |
| 5xx / 408 / 429 / 网络或资源失败 | 5 秒 → 15 秒 → 60 秒封顶；离线时暂停 |
| 完整但无效的 200 | 停止，避免协议不匹配时重复请求 |
| 200 body 截断、读取超时 | 作为网络错误重试 |
| 已收到 401 等错误后 body 读取失败 | 保留原 HTTP 状态，不能变成可重试网络错误 |
| 断网、重新配网、显式刷新后的迟到结果 | 丢弃，不能覆盖新状态或校时 |

一次只允许一个未完成请求；worker 优先级 2、core 0、栈 8192 字节。两个指针队列容量各 1。
缺省关闭时不创建 worker 或队列；首次启用后 worker 保留到重启，空闲时阻塞等待。
大型配置、结果、16 KiB 响应缓冲在 PSRAM，分配失败按退避处理；HTTP/TLS 的内部分配
沿用 SDK；本工程已配置 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`，TLS 使用 PSRAM。
后台任务避免阻塞主任务，但 HTTPS 与语音连接并发仍会消耗 CPU/堆，需实板观察
AFE 积压、音频连续性、内部堆余量和任务栈水位。

单次网络操作超时 5 秒，响应体另设 15 秒预算，分 512 字节读取，最多 16384 字节。
取消在网络步骤之间检查；已经阻塞的 DNS/TCP/TLS/读取调用需返回后才释放，因此不承诺
“整次握手严格 15 秒内结束”。应用主任务和音频任务不等待 HTTP。

HTTPS 使用证书包和主机名验证，未添加跳过证书验证开关。现有 sdkconfig 没有启用
`CONFIG_MBEDTLS_HAVE_TIME_DATE`（证书时间有效期检查）；本轮保留原 SDK 配置。
量产时应结合可信时间来源启用并联调，不能依赖尚未完成的第一次 HTTPS 握手来解除证书时间问题。

日志默认只输出状态码和固定原因，不输出 PSK、MQTT password、WS token 或响应正文。
初始化将 `HTTP_CLIENT` 日志限制到 INFO，因为 IDF 的 DEBUG 日志会打印完整请求头。

按用户要求，本机私有头文件已设置 `DEVICE_HANDSHAKE_LOG_WS_TOKEN 1`：每次成功握手额外打印
`DEV ONLY websocket.token=<平台返回的完整语音 token>`，用于串口联调。PSK/MQTT password 仍不输出。
这份私有开关不提交 Git，仓库缺省为 0；联调结束改为 0 后重新编译，分享日志前删除该行。

排查语音网关的 `API Key 无效或已吊销` 时，`ws_service` 在发送鉴权前记录本次实际
使用的 token 来源、长度和 SHA-256 指纹（`Auth token: ...`），不输出原文或 auth JSON。
平台模式来源应为 `HTTP/handshake/websocket.token`；仅显式关闭握手时才应看到
`NVS/korvo_cfg/acos_token`。来源正确不等于认证成功，仍以网关成功事件为准。
平台模式不会覆盖原来的 NVS key，也不会将短期 token 持久化。

### 语音凭据生命周期

- TTL 使用单调时钟，以请求开始时刻为保守基准，扣除网络和主任务排队时间，避免校时延长有效期。
- 唤醒时握手未完成：主任务继续显示连接状态并等待后台结果，不建立旧 key 的 WS。
- 剩余时间少于安全窗口：发起一次续期，后续轮询不会重置请求代次/退避。
- URL/token 整组复制到 WS 服务约 3 KiB 的 PSRAM 快照。URL 改变时重建已停止的 transport，
  避免组件 URI 更新遗留旧端口/路径；同地址换 token 不重建 transport。
- auth JSON 在 PSRAM 临时构造，支持 2048 字节 token 和 JSON 转义，发送后清零释放。
- 已建立的语音会话不因本地 TTL 到点被主动打断；下一次建连再检查。平台若主动报凭据过期，
  进入鉴权恢复。正在通话期间 token 到期的服务端规则仍待平台确认。
- 每次主动唤醒最多因明确鉴权错误或鉴权等待超时重新握手一次，沿用 5 秒重连间隔；
  新凭据仍被拒绝则结束本次会话，等人工唤醒再试，不无限重连。HTTP 永久错误不能被此流程解除。
- 普通 TLS/网络连接错误仍走现有网络恢复，不把连接失败一律当成 token 失效。

## 7. 代码位置和扩展约定

| 文件 | 职责 |
| --- | --- |
| `main/device_handshake.h/.c` | 纯 C 配置/响应解析、UTC、错误码、重试及网络代次 |
| `main/device_handshake_http.h/.c` | 一次 HTTP 请求交换，限长、读取、取消及释放；可注入测试传输 |
| `main/device_handshake_service.h/.c` | NVS、私有配置、ESP HTTP 适配、worker 和主任务快照 |
| `main/device_handshake_local_config.example.h` | 不含真实凭据的开发配置模板 |
| `main/app_main.c` | 初始化/poll、建连前凭据门禁、有限鉴权恢复 |
| `main/ws_service.h/.c` | 独立连接凭据副本、动态 URL、鉴权发送与错误事件 |
| `main/proto_service.h/.c` | 凭据长度上限、安全 JSON 转义、已知鉴权错误分类 |
| `tests/device_handshake_checks.c` | 直接运行生产解析、HTTP 交换和退避策略的主机回归 |
| `tests/device_auth_checks.c` | 运行生产凭据准备/复制/发送及主任务门禁函数，transport 使用主机替身 |

握手服务所有接口都只由应用主任务调用；自动续期使用 `renew_credentials()`，不能误用会解除永久错误的 `refresh()`。
网络回调只递增原子 epoch，不读写配置或进行阻塞操作；worker 使用独立配置副本，
经有界队列转移结果所有权。即使两次 poll 之间发生短暂断网重连，也会丢弃旧结果。
`get_configuration()` 的指针只借用至下一次 poll/refresh，跨任务消费者必须复制并建立自己的
生命周期规则，不能保存该指针后继续使用已释放的 token。

## 8. 验证和下一步联调

主机测试使用现有 TCC 和工程锁定的 cJSON；脚本从当前工程读取生产代码：

```powershell
python scripts/check_handshake.py --cc D:\ESP32-ymos-1\work\host-tools\tcc\tcc.exe
python scripts/check_device_auth.py --cc D:\ESP32-ymos-1\work\host-tools\tcc\tcc.exe
python scripts/check_audio.py --cc D:\ESP32-ymos-1\work\host-tools\tcc\tcc.exe
```

测试覆盖配置、边界/截断/非法 JSON、正式/测试 ID、UTC/闰年、字段归属、分段 HTTP、超时、
中断、重定向、永久错误不重试、Wi-Fi 抖动不清除退避、旧结果失效和显式刷新。
假传输验证不代表已经连接真实平台；NVS 注入、HTTPS、RTOS 调度和真实音频并发需实板联调。

本轮固件烧录后应观察（以下为验收步骤，不代表已完成实板测试）：

1. 正确配置：`HTTP handshake started` → `HTTP handshake ready; ... (RAM only)`，
   同时由平台确认登记设备、型号、固件版本和 MAC 正确。唤醒后应出现
   `Connecting WebSocket; credentials=device handshake` 和
   `Auth token: source=HTTP/handshake/websocket.token ...`，随后才是 `WebSocket authenticated`。
2. 错 PSK：HTTP 401 后停止；断开/恢复 Wi-Fi 不能造成循环鉴权。
3. 未登记/禁用：404/403 后停止。
4. 临时网络或 5xx：按 5/15/60 秒退避；恢复后成功。
5. 请求中断 Wi-Fi，再恢复：旧结果丢弃，只接受新请求。
6. 现有唤醒、正常对话、打断、屏幕菜单、BOOT/触屏配网回归；观察内部堆和任务栈余量。
7. 联网后立即唤醒：先等待 HTTP，再打开语音；不能出现旧 NVS token 的发送日志。
8. token 接近过期后再次唤醒：先续期；有效期充足时再次唤醒不增加 HTTP 请求。
9. 语音鉴权拒绝：最多一次重新握手；再次拒绝后结束会话，无无限重连。

2026-11-10 已新增独立 MQTT 服务，实现 MQTT 3.1.1、连接/订阅返回码判断、首次和周期 status、
任务去重及重连策略；文档里的 SUBACK 0 与 QoS 1 要求、文本任务到语音网关的请求协议、
WS token 的传递方式及成功事件仍需与平台对齐。本轮不假定旧 ACOS 的 MQTT 原文转发协议适用于新平台。

### 2026-11-09 初次 HTTP 移植记录（历史版本，尚未接入动态 WS 凭据）

- HTTP 协议/请求交换/退避主机测试：981 项检查通过，直接运行生产代码。
- 原语音回归通过：413 项响应流检查、97874 项输入分包/暂停恢复检查、
  341 项交接策略检查、268 项应用交接检查；不代表真实声学和 RTOS 并发已验证。
- ESP-IDF 6.1 / ESP32-S3 构建成功；镜像 2,550,400 字节，3 MiB 应用分区剩余约 19%。
  已校验最终镜像包含本机启用配置，PSK 不在可提交文件中，私有头文件被 Git 忽略。
- 固件 SHA-256：`585080c6dc3c818eb9f9b81f37122705f08f35848d17f301ae6ded1db50aac80`。
- 构建中 Windows 的 ldgen 命令包装子进程挂起；使用生成规则中的同一 SDK 命令
  直接调用 Python，经临时 Ninja 构建文件完成链接和分区检查，未修改源码构建规则或 SDK。
  过程记录在本地忽略目录 `work/http-handshake-20261109/`。
- 当时平台端探测为 HTML 404；后续用户实机日志已握手成功，见本文第 1 节。该初次移植未烧录、未推送 GitHub。

### 2026-11-09 动态语音凭据接入验证

- 握手解析/HTTP/TTL/退避：1060 项主机检查通过。
- 生产凭据准备、主任务门禁、长 token/JSON 转义、独立副本和有限鉴权恢复：131 项主机检查通过。
  transport/NVS/时钟使用替身，不代表平台实测。
- 原语音回归：413 项响应流、97874 项输入上传、341 项交接策略、268 项主任务交接检查通过。
  原 `process_reply_offer`、`resume_listening`、`interrupt_response` 函数与本轮修改前一致。
- ESP-IDF 6.1 / ESP32-S3 编译成功；`build/vocat_korvo_agent.bin` 为 2,553,728 字节，应用分区剩余约 19%。
- 此次本机固件已启用返回 token 的串口日志；SHA-256：
  `0cbbbe1c9d80f72647f7acdcd7d22b0e64b5c5de2f0646854b7192abf9c71c73`。
- 未烧录、未验证真实平台的语音 token 鉴权、未推送 GitHub。
- 本轮基线副本、构建日志和结果在本地忽略目录 `work/device-ws-auth-20261109/`。
