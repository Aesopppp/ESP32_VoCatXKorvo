#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(bytes, caps) malloc(bytes)
