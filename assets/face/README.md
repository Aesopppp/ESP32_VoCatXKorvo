# VoCat 基础表情资源

本目录保存基础 EAF 文件，保留原资源包的四个槽位，不包含开心、难过等情绪资源。
文件复制自已运行的 `ESP32_XChatbot_v1.2` 工程的 `espressif2022/esp_emote_assets 0.1.2`，
未修改图像内容；固件仅使用资源，不引入该工程的 C++ 应用或 GFX 刷屏线程。

| 文件 | 有效帧 | 尺寸 | 本工程用途 |
| --- | ---: | --- | --- |
| `neutral.eaf` | 88 | 125×160 | 待机/聆听/回答的双眼基础动画，右眼水平镜像 |
| `winking.eaf` | 14 | 125×160 | 兼容原资源包保留，目前不播放；取消回答直接使用监听画面 |
| `confused.eaf` | 77 | 125×160 | 等待平台回复的思考动画 |
| `listen.eaf` | 50 | 100×46 | 聆听时顶部、回答时底部的动态状态条 |

全部采用 20fps（50ms/帧）。EAF 文件末尾还有一个 `_C` 元信息记录，不是图像帧。
状态条仅表示业务状态，不是麦克风或扬声器的实时波形。
取消和监听共用同一动画时间轴，不显示独立打断画面。本次仅改 UI 映射，资源包内容/版本不变，
已烧录的资源可以继续使用。

## 来源与许可记录

- 上游资源项目：<https://github.com/espressif2022/esp_emote_assets>。
- 资源包清单声明 `license: Apache-2.0`，版本 0.1.2，提交
  `29aa5259ac22651b8636683698c021482050b206`，路径 `emoji_large/`。
- 文件 SHA-256 固定在 `manifest.json`，构建时校验，不依赖另一工程目录。
- EAF 格式及 BGRA → RGB565 规则参考 `espressif2022/esp_emote_gfx 3.0.5` 的
  `src/lib/eaf/gfx_eaf_dec.c/.h`（Espressif Systems (Shanghai) CO LTD，Apache-2.0）。
- 本工程 `main/ui_animation.c` 实现仅支持上述固定资源的 8 位调色板/RLE 子集，
  无动态分配，增加边界校验；不复制原组件的 Huffman、JPEG、热压缩或绘图引擎。
- 保留 Apache-2.0 全文于 `LICENSE-Apache-2.0.txt`。此记录说明组件声明和文件来源，
  不替代对具体图像内容权利及整个产品其他依赖的商业许可审核。

## 构建与升级约定

`scripts/generate_face_assets.py` 用 Python 标准库将四个 EAF 原文件打包为
`build/ui_assets.bin`，格式 `VFACE01`，本版 486,084 字节。
CMake 在正常 build 时生成，在正常 flash 时加入烧录清单。
资源分区 `ui_assets` 位于 `0x800000`，容量 1MiB；已有 app0/app1/model/NVS 偏移不变。

启动后由 UI 任务只读映射并校验资源，每帧复用固定 PSRAM 图像缓冲。
缺失资源、校验失败或不支持的压缩格式会使用静态眼睛，菜单/语音仍可用。
运行中不会访问 SD 卡、下载资源、解码整个动画到内存或反复分配帧缓冲。

修改动画文件时同步检查解码格式、帧尺寸及 `manifest.json` 的哈希，运行主机回归并重新打包。
修改资源 ID 顺序、容器或解码语义时须升级资源版本及相应固件，不能静默沿用旧版本。
首次部署必须写入新分区表和资源；仅 app-flash 或仅应用 OTA 不会补齐缺失资源。
本轮没有新增资源 OTA 下载器或双分区资源切换，量产远程更新资源需另行设计。
