#pragma once
/* 复制为 device_mqtt_local_config.h（Git 忽略）。
 * 新文档只定义 MQTT content，未定义请求 TTS 的 WS 消息。
 * 平台确认仍支持“MQTT 原文转发 WS”后才设为 1；不猜造平台接口。
 * 关闭时仍连接/订阅/状态上报，任务有界等待，不发虚假的已播报 ACK。 */
#define DEVICE_MQTT_FORWARD_TASK_TO_WS 0
/* CA 放 main/certs/device_mqtt_ca.pem，必须是平台重新签发的有效根证书。 */
