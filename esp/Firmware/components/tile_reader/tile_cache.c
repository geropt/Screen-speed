#include "tile_cache.h"
#include "tile_reader.h"
#include "tile_config.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "TILE_CACHE";

/* A trip needs at most ~128 entries and ~305 KB; both limits are generous.
 * Negative entries take a slot but no bytes. */
#define CACHE_MAX_ENTRIES  512
#define CACHE_BUDGET_BYTES (2 * 1024 * 1024)

typedef struct {
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t *data;     /* NULL when the tile does not exist on the card */
    uint32_t len;
    uint32_t used;     /* logical clock stamp, for LRU eviction */
    bool     valid;    /* slot is occupied */
} tile_entry_t;

/* The index lives in PSRAM too. As a static array it was 12 KB of internal RAM,
 * and internal RAM is exactly what LVGL needs: its draw buffers are ~211 KB of
 * MALLOC_CAP_DMA, which PSRAM cannot serve. Those 12 KB were enough to push
 * `buf2` past the edge and turn boot into an assert loop at
 * waveshare_amoled_lcd_port.cpp:306. Nothing this file allocates may come from
 * the internal heap. */
static tile_entry_t *s_entries;
static bool s_index_failed;
static uint32_t s_clock;
static uint32_t s_count;
static size_t   s_bytes;
static uint32_t s_hits;
static uint32_t s_misses;

/* Used only if the index itself cannot be allocated: one tile is held at a
 * time, freed on the next miss. Safe because a returned buffer is consumed
 * inside a single scan_tile_for_match() call, before the next lookup. */
static uint8_t *s_solo_data;

static bool ensure_index(void)
{
    if (s_entries)
        return true;
    if (s_index_failed)
        return false;
    s_entries = heap_caps_calloc(CACHE_MAX_ENTRIES, sizeof(tile_entry_t),
                                 MALLOC_CAP_SPIRAM);
    if (!s_entries) {
        s_index_failed = true;
        ESP_LOGW(TAG, "no PSRAM for the tile index, running uncached");
        return false;
    }
    return true;
}

static inline int32_t filename_coord(int32_t origin_e7)
{
    return origin_e7 / FILENAME_SCALE;
}

/* Drop the least recently used entry. Prefers one that actually holds bytes --
 * evicting a negative entry frees a slot but no memory, which is useless when
 * the budget is what we are over. */
static void evict_one(bool need_bytes)
{
    int victim = -1;
    uint32_t oldest = UINT32_MAX;

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!s_entries[i].valid)
            continue;
        if (need_bytes && s_entries[i].data == NULL)
            continue;
        if (s_entries[i].used < oldest) {
            oldest = s_entries[i].used;
            victim = i;
        }
    }
    /* Nothing holds bytes: fall back to any entry so a slot frees up. */
    if (victim < 0 && need_bytes) {
        evict_one(false);
        return;
    }
    if (victim < 0)
        return;

    if (s_entries[victim].data) {
        s_bytes -= s_entries[victim].len;
        heap_caps_free(s_entries[victim].data);
    }
    memset(&s_entries[victim], 0, sizeof(s_entries[victim]));
    s_count--;
}

static int find_free_slot(void)
{
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!s_entries[i].valid)
            return i;
    }
    return -1;
}

const uint8_t *tile_cache_get(int32_t origin_lat_e7, int32_t origin_lon_e7,
                              size_t *out_len)
{
    bool indexed = ensure_index();

    if (indexed) {
        for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
            if (s_entries[i].valid &&
                s_entries[i].lat_e7 == origin_lat_e7 &&
                s_entries[i].lon_e7 == origin_lon_e7) {
                s_entries[i].used = ++s_clock;
                s_hits++;
                if (out_len)
                    *out_len = s_entries[i].len;
                return s_entries[i].data;   /* NULL here means "known absent" */
            }
        }
    }

    s_misses++;

    char filename[96];
    snprintf(filename, sizeof(filename),
             TILE_PATH "/%" PRId32 "/tile_%" PRId32 "_%" PRId32 ".bin",
             filename_coord(origin_lat_e7), filename_coord(origin_lat_e7),
             filename_coord(origin_lon_e7));

    uint8_t *buf = NULL;
    uint32_t len = 0;

    FILE *f = fopen(filename, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size > 0 && size < (long)CACHE_BUDGET_BYTES) {
                rewind(f);
                /* PSRAM explicitly: a plain malloc of this size lands in
                 * internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL = 16 KB). */
                buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
                if (buf) {
                    if (fread(buf, 1, (size_t)size, f) == (size_t)size) {
                        len = (uint32_t)size;
                    } else {
                        heap_caps_free(buf);
                        buf = NULL;
                    }
                }
            }
        }
        fclose(f);
        /* A tile that exists but could not be read is not cached at all, so a
         * later fix retries instead of remembering a wrong "absent". */
        if (!buf) {
            if (out_len)
                *out_len = 0;
            return NULL;
        }
    }
    /* buf == NULL && f == NULL: the tile genuinely does not exist. Cache that,
     * or the 8 neighbours of every border tile cost 8 failed fopen() per fix. */

    if (!indexed) {
        /* Degraded mode: hold just this tile, release the previous one. */
        if (s_solo_data)
            heap_caps_free(s_solo_data);
        s_solo_data = buf;
        if (out_len)
            *out_len = len;
        return buf;
    }

    while (s_count >= CACHE_MAX_ENTRIES)
        evict_one(false);
    while (buf && s_bytes + len > CACHE_BUDGET_BYTES)
        evict_one(true);

    int slot = find_free_slot();
    if (slot < 0) {
        /* Should not happen after the eviction loop; do not leak the buffer. */
        if (buf)
            heap_caps_free(buf);
        if (out_len)
            *out_len = 0;
        return NULL;
    }

    s_entries[slot].lat_e7 = origin_lat_e7;
    s_entries[slot].lon_e7 = origin_lon_e7;
    s_entries[slot].data   = buf;
    s_entries[slot].len    = len;
    s_entries[slot].used   = ++s_clock;
    s_entries[slot].valid  = true;
    s_count++;
    s_bytes += len;

    if (out_len)
        *out_len = len;
    return buf;
}

void tile_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                      size_t *out_bytes, uint32_t *out_entries)
{
    if (out_hits)    *out_hits = s_hits;
    if (out_misses)  *out_misses = s_misses;
    if (out_bytes)   *out_bytes = s_bytes;
    if (out_entries) *out_entries = s_count;
    ESP_LOGD(TAG, "hits=%" PRIu32 " misses=%" PRIu32 " bytes=%u entries=%" PRIu32,
             s_hits, s_misses, (unsigned)s_bytes, s_count);
}
