# MQTT 信任根

将平台提供的新版 ca.crt 以 PEM 文本保存为本目录的 device_mqtt_ca.pem，
然后重新执行 idf.py reconfigure 和 idf.py build。

该 CA 必须包含 CA 约束和 keyCertSign 用途，服务端证书 SAN 必须匹配
HTTP 握手返回的 MQTT endpoint 中的主机。不能用 HTTPS 443 的证书替代。
没有此文件时固件会明确禁用 MQTT，其他功能继续工作，不使用不安全连接。

CA 是公开信任根，可以随源代码发布；不要放服务端私钥、PSK 或设备密钥。
