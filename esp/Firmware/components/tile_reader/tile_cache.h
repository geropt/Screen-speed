#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whole-file cache for map tiles, held in PSRAM.
 *
 * Every fix scans up to 9 tiles, and a real drive keeps landing on the same
 * ones: replaying the six reference logs, a whole trip touches only 42-128
 * distinct tiles (under 305 KB) while issuing 3100-5500 open/read/close cycles.
 * Caching the raw file bytes turns all but the first read of each tile into a
 * pointer lookup.
 *
 * The buffers come from PSRAM explicitly. A plain malloc() would not use it:
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL keeps allocations under 16 KB in internal
 * RAM, and every tile is smaller than that (median 0.7 KB, largest 14.5 KB).
 *
 * @param origin_lat_e7 tile origin latitude, 1e7 fixed point
 * @param origin_lon_e7 tile origin longitude, 1e7 fixed point
 * @param out_len       receives the file length in bytes
 * @return pointer to the tile bytes, valid until the entry is evicted, or NULL
 *         if the tile does not exist on the card
 */
const uint8_t *tile_cache_get(int32_t origin_lat_e7, int32_t origin_lon_e7,
                              size_t *out_len);

/**
 * @brief Counters for the periodic health log.
 *
 * @param out_hits    lookups served from memory
 * @param out_misses  lookups that hit the SD card
 * @param out_bytes   tile bytes currently held in PSRAM
 * @param out_entries slots in use, including negative (tile-absent) entries
 */
void tile_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                      size_t *out_bytes, uint32_t *out_entries);

#ifdef __cplusplus
}
#endif
