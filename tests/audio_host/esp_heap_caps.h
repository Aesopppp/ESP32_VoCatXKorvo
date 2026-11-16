#pragma once
#include <stddef.h>
#undef heap_caps_malloc
#define MALLOC_CAP_SPIRAM 2
void *test_heap_caps_malloc(size_t bytes, unsigned caps);
#define heap_caps_malloc(bytes, caps) test_heap_caps_malloc(bytes, caps)
