#ifndef DEVICE_HANDSHAKE_LOCAL_CONFIG_H
#define DEVICE_HANDSHAKE_LOCAL_CONFIG_H

/* 复制为 device_handshake_local_config.h 后再填写；私有文件不提交 Git。
 * 平台发号后将 enabled 改为 true。server_url 为 HTTP 服务基础地址，不是语音 WS 地址。
 * 下列值只是占位符，正式 ID 和原始 PSK 必须由平台提供。
 * JSON 内双引号需要写成 \"；复杂凭据建议用文档说明的 NVS JSON 注入方式。 */
#define DEVICE_HANDSHAKE_LOCAL_JSON \
    "{\"enabled\":false," \
    "\"server_url\":\"https://device-server.example.com\"," \
    "\"device_id\":\"REPLACE-WITH-REGISTERED-ID\"," \
    "\"device_model\":\"esp32s3\"," \
    "\"psk\":\"REPLACE-WITH-DEVICE-PSK\"," \
    "\"allow_insecure_http\":false}"

/* 可选：新建语音连接前，剩余有效期不足此值就续期；默认 300 秒。
 * #define DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS 300u
 * 新签发 token 的 TTL 也必须大于此窗口及请求耗时。 */

/* 可选串口诊断，默认关闭。仅本地联调需要查看返回 token 时设为 1。
 * #define DEVICE_HANDSHAKE_LOG_WS_TOKEN 0
 * 开启后串口日志含短期语音凭据，分享日志前脱敏，量产固件保持关闭。 */

#endif
