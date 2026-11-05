#ifndef BOARD_SERVICE_H
#define BOARD_SERVICE_H

#include "esp_err.h"

/* system_init 调用：开启音频电源，关闭背光，建立音频 I2C 并确认 Codec。
 * 总线由本模块持有直到重启；音频模块借用，不得删除。重复调用为 no-op。
 */
esp_err_t board_service_init(void);

#endif
