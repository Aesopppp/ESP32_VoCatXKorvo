idf_component_register(
    SRCS
        "app_main.c"
        "board_service.c"
        "display_service.c"
        "touch_service.c"
        "ui_service.c"
        "ui_face.c"
        "ui_animation.c"
        "ui_face_assets.c"
        "vocat_ui_font_20.c"
        "button_service.c"
        "app_state.c"
        "event_bus.c"
        "audio_in.c"
        "wake_word.c"
        "audio_out.c"
        "ws_service.c"
        "response_stream.c"


        "opus_codec.c"

        "proto_service.c"
        "led_service.c"
        "wifi_service.c"
        "cfg_service.c"
        "device_handshake.c"
        "device_handshake_http.c"
        "device_handshake_service.c"
        "device_mqtt_protocol.c"
        "device_mqtt_storage.c"
        "device_mqtt_service.c"
        "log_service.c"
        "provisioning_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_codec_dev
        esp-sr
        opus_codec
        esp_websocket_client
        mqtt
        esp_wifi
        nvs_flash
        esp_timer
        esp_system
        esp_netif
        esp_http_client
        esp_https_ota
        driver
        freertos
        log
        esp_hw_support
        spi_flash
        esp_psram
        mbedtls
        cjson
        esp_http_server
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_uart
        esp_ringbuf
        esp_lcd
        esp_partition
        esp_lcd_st77916
        lvgl
        esp_driver_spi
        esp_driver_ledc
)

# 8883 使用平台私有 CA，与 HTTPS/WSS 公共根证书包不同。没有 CA 时只禁用
# MQTT，不伪造证书、不跳过校验；放入证书后重新 reconfigure/build 即可。
set(device_mqtt_ca "${CMAKE_CURRENT_SOURCE_DIR}/certs/device_mqtt_ca.pem")
if(EXISTS "${device_mqtt_ca}")
    target_add_binary_data(${COMPONENT_LIB} "${device_mqtt_ca}" TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_MQTT_HAS_CA=1)
else()
    message(WARNING "MQTT disabled until main/certs/device_mqtt_ca.pem is supplied")
endif()

# Suppress warnings from IDF headers in third-party code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
