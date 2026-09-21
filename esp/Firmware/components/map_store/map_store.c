#include "map_store.h"
#include "tile_cache.h"
#include "map_capsule.h"

#include <stdio.h>
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "MAP_STORE";

/* Buffer del payload que se entrega al matcher cuando el backend es cápsula.
 * Ownership: válido hasta la próxima llamada a fetch, igual que con la caché de
 * tiles. El tile más grande del dataset actual mide 14,5 KiB; 32 KiB deja margen sin
 * comprometer el presupuesto medido en P01. */
#define CAPSULE_PAYLOAD_MAX (32 * 1024)

static struct {
    bool     inited;
    bool     available;
    map_backend_t backend;
    FILE         *cap_file;
    map_capsule_t capsule;
    bool          capsule_open;
    uint8_t      *payload_buf;
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

/* Lectura por rangos sobre el archivo de la cápsula. */
static size_t capsule_read_at(void *ctx, uint64_t offset, uint8_t *out, size_t len)
{
    (void)ctx;
    if (!s.cap_file) {
        return 0;
    }
    if (fseek(s.cap_file, (long)offset, SEEK_SET) != 0) {
        return 0;
    }
    return fread(out, 1, len, s.cap_file);
}

esp_err_t map_store_set_backend(map_backend_t backend, const char *capsule_path)
{
    map_store_init();

    /* Cerrar lo anterior antes de abrir lo nuevo. */
    if (s.cap_file) {
        fclose(s.cap_file);
        s.cap_file = NULL;
    }
    s.capsule_open = false;

    if (backend == MAP_BACKEND_CAPSULE) {
        if (!capsule_path) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!s.payload_buf) {
            s.payload_buf = malloc(CAPSULE_PAYLOAD_MAX);
            if (!s.payload_buf) {
                return ESP_ERR_NO_MEM;
            }
        }
        s.cap_file = fopen(capsule_path, "rb");
        if (!s.cap_file) {
            ESP_LOGE(TAG, "no se pudo abrir la capsula %s", capsule_path);
            return ESP_ERR_NOT_FOUND;
        }
        fseek(s.cap_file, 0, SEEK_END);
        long size = ftell(s.cap_file);
        capsule_err_t cerr = map_capsule_open(&s.capsule, capsule_read_at, NULL,
                                             (uint64_t)size);
        if (cerr != CAPSULE_OK) {
            ESP_LOGE(TAG, "capsula invalida: %s", capsule_err_name(cerr));
            fclose(s.cap_file);
            s.cap_file = NULL;
            return ESP_ERR_INVALID_STATE;
        }
        s.capsule_open = true;
        ESP_LOGI(TAG, "capsula abierta: %" PRIu32 " celdas, region %s",
                 s.capsule.index_count, s.capsule.manifest.region);
    }

    s.backend = backend;
    /* El contenido puede no ser el mismo mapa: generación nueva. */
    s.generation++;
    return ESP_OK;
}

map_backend_t map_store_backend(void)
{
    return s.backend;
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

    if (s.backend == MAP_BACKEND_CAPSULE) {
        if (!s.capsule_open || !s.payload_buf) {
            s.io_errors++;
            return NULL;
        }
        capsule_index_entry_t entry;
        capsule_err_t cerr = map_capsule_find(&s.capsule, origin_lat_e7,
                                             origin_lon_e7, &entry);
        if (cerr == CAPSULE_ERR_NOT_FOUND) {
            s.absent++;
            return NULL;
        }
        if (cerr != CAPSULE_OK) {
            s.io_errors++;
            return NULL;
        }
        cerr = map_capsule_read_payload(&s.capsule, &entry, s.payload_buf,
                                       CAPSULE_PAYLOAD_MAX);
        if (cerr != CAPSULE_OK) {
            /* CRC malo o lectura corta: no se le dan bytes dañados al matcher. */
            s.io_errors++;
            return NULL;
        }
        if (out_len) {
            *out_len = entry.payload_len;
        }
        return s.payload_buf;
    }

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
