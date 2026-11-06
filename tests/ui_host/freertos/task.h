#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,int);
void vTaskDelay(unsigned);
