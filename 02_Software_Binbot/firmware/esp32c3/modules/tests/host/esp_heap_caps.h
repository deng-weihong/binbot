#pragma once

#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 0
static inline void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}
static inline void heap_caps_free(void *memory)
{
    free(memory);
}

