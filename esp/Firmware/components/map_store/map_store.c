#include "map_store.h"
#include "tile_cache.h"

#include <string.h>

static struct {
    bool     inited;
    bool     available;
    uint64_t generation;
    uint32_t fetches;
    uint32_t absent;
    uint32_t io_errors;
} s;

void map_store_init(void)
{
    if (s.inited) {
        return;
    }
    memset(&s, 0, sizeof(s));
    s.inited = true;
    /* generation 0 = nunca hubo mapas. La primera disponibilidad la lleva a 1. */
}

void map_store_set_available(bool available)
{
    map_store_init();
    if (s.available == available) {
        return;
    }
    s.available = available;
    if (available) {
        /* Generación nueva: el contenido puede ser de otra tarjeta, así que
         * cualquier resultado calculado antes queda marcado como viejo. */
        s.generation++;
    }
}

bool map_store_is_available(void)
{
    return s.available;
}

uint64_t map_store_generation(void)
{
    return s.generation;
}

const uint8_t *map_store_fetch(void *ctx, int32_t origin_lat_e7,
                              int32_t origin_lon_e7, size_t *out_len)
{
    (void)ctx;
    if (out_len) {
        *out_len = 0;
    }
    if (!s.available) {
        /* Sin mapas no se intenta abrir nada. Antes el matcher hacía las nueve
         * aperturas igual y fallaban una por una. */
        return NULL;
    }
    s.fetches++;
    const uint8_t *buf = tile_cache_get(origin_lat_e7, origin_lon_e7, out_len);
    if (!buf) {
        s.absent++;
    }
    return buf;
}

map_tile_source_t map_store_tile_source(void)
{
    map_tile_source_t src = {
        .fetch = map_store_fetch,
        .ctx = NULL,
        .generation = s.generation,
    };
    return src;
}

void map_store_get_stats(map_store_stats_t *out)
{
    if (!out) {
        return;
    }
    out->generation = s.generation;
    out->fetches = s.fetches;
    out->absent = s.absent;
    out->io_errors = s.io_errors;
}
