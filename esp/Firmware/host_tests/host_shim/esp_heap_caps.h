/* Host stand-in for esp_heap_caps.h.
 *
 * Maps the capability allocators onto plain libc so the tile cache can run
 * under ASan/UBSan on the host. Capability flags are kept as distinct bits so
 * code that inspects them still compiles, but the host has no PSRAM/DMA
 * distinction: any test that depends on allocation *capabilities* (rather than
 * on cache logic) must run on the device.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_EXEC     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  (1 << 12)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    return realloc(ptr, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

/* The host has no fixed budget; report a large constant so callers that only
 * log the value keep working. Never assert on these in a host test. */
static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return (size_t)16 * 1024 * 1024;
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return (size_t)4 * 1024 * 1024;
}

#ifdef __cplusplus
}
#endif
