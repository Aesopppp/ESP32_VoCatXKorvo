/** ESP-VoCat v1.2 固定板型；网络/业务参数沿用 YMOS。 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 音频控制总线。地址统一使用 7-bit，传入 codec_dev 时左移一位。 */
#define BSP_I2C_SDA_PIN          GPIO_NUM_2
#define BSP_I2C_SCL_PIN          GPIO_NUM_1
#define BSP_I2C_PORT             I2C_NUM_0
#define BSP_I2C_FREQ_HZ          400000
#define ES7210_I2C_ADDR          0x40
#define ES8311_I2C_ADDR          0x18

/* TX 标准 I2S，RX 四槽 TDM；在 codec_dev 中取 slot0/slot1 作为 M/R。
 * 16 kHz 硬件采样与 AFE/Opus 一致，避免额外重采样和参考通道延迟。
 */
#define BSP_I2S_PORT             I2S_NUM_0
#define BSP_I2S_MCLK_PIN         GPIO_NUM_42
#define BSP_I2S_BCLK_PIN         GPIO_NUM_40
#define BSP_I2S_WS_PIN           GPIO_NUM_39
#define BSP_I2S_DOUT_PIN         GPIO_NUM_41
#define BSP_I2S_DIN_PIN          GPIO_NUM_3
#define BSP_I2S_SAMPLE_RATE      16000
#define BSP_AFE_INPUT_FORMAT     "MR"
#define BSP_AFE_INPUT_CHANNELS   2
#define BSP_MIC_GAIN_DB          30.0f
/* 与已运行的 VoCat Codec 默认增益一致；后续根据实板削顶统计再调。 */
#define BSP_REFERENCE_GAIN_DB    30.0f

#define BSP_CODEC_POWER_PIN     GPIO_NUM_48  /* 高电平使能 v1.2 音频电源 */
#define BSP_PA_EN_PIN           GPIO_NUM_15  /* 功放使能由 ES8311 驱动管理 */
#define BSP_POWER_CTRL_PIN      GPIO_NUM_9   /* 保持 VoCat 原板级低电平 */
#define BSP_LCD_BACKLIGHT_PIN   GPIO_NUM_44  /* 本版不接屏幕，保持关闭 */
#define BSP_BOOT_BUTTON_PIN     GPIO_NUM_0   /* 低电平按下，运行中长按 3 秒配网 */

/* Wi-Fi / NVS：沿用原工程配置；ACOS 本机凭据由配置服务一次性写入 NVS。 */
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define WIFI_MAX_RETRY           5
#define DEFAULT_WIFI_SSID        ""
#define DEFAULT_WIFI_PASSWORD    ""

/* ===========================================================================
 * ACOS
 * ========================================================================= */
#define WS_SERVICE_DEFAULT_HOST   "119.29.215.114"
#define ACOS_WS_URL              "wss://119.29.215.114/v1/realtime"
/* 密钥不写进公共头文件，见 acos_local_config.h.example。 */
#define DEFAULT_ACOS_TOKEN       ""
#define ACOS_OPUS_BITRATE        24000   /* - Opus 24 kbps */
/* -: ACOS  20ms/320 - */
#define ACOS_OPUS_FRAME_MS_UPLOAD   20
/* -: 60ms Opus  */
#define ACOS_OPUS_FRAME_MS_DOWNLOAD 60
#define ACOS_OPUS_FRAME_MS          ACOS_OPUS_FRAME_MS_DOWNLOAD /* - */
#define ACOS_INPUT_MAX_MS        20000   /* - 20 s */
#define ACOS_AUTH_TIMEOUT_MS     10000   /* - __proxy_connected__ - */

/* API Key -: sk-{enterprise}-{32 chars} */
#define ACOS_TOKEN_PREFIX        "sk-"
#define ACOS_TOKEN_MIN_LEN       12

/* ===========================================================================
 * Opus
 * ========================================================================= */
#define OPUS_SAMPLE_RATE         16000
#define OPUS_CHANNELS            1
#define OPUS_COMPLEXITY          2       /* 0-10, lower = less CPU (was 4, dropped to reduce LISTENING overhead) */

/* ===========================================================================
 * VAD -
 * ========================================================================= */
#define VAD_FRAME_SIZE_MS        30      /* VAD  30 ms */
#define VAD_SILENCE_TIMEOUT_MS   800     /*  - */
#define VAD_MIN_UTTERANCE_MS     300     /* */

/* ===========================================================================
 * -
 * ========================================================================= */
#define AUDIO_IN_QUEUE_SIZE      8
#define AUDIO_OUT_QUEUE_SIZE     8
#define AUDIO_CHUNK_SIZE         (BSP_I2S_SAMPLE_RATE * VAD_FRAME_SIZE_MS / 1000 * 2)  /* 16-bit PCM, 30ms = 960 bytes */
#define AUDIO_WS_QUEUE_SIZE      16

/* ===========================================================================
 * FreeRTOS - * ========================================================================= */
#define TASK_STACK_AUDIO_IN      4096
#define TASK_STACK_AUDIO_OUT     4096
#define TASK_STACK_WS            6144
#define TASK_STACK_PROTO         4096
#define TASK_STACK_WIFI          4096

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CONFIG_H */


