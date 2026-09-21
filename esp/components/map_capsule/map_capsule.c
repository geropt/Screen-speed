#include "map_capsule.h"

#include <string.h>

/* ---------- CRC-32 ---------- */

uint32_t capsule_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return ~crc;
}

uint32_t capsule_crc32(const uint8_t *data, size_t len)
{
    return capsule_crc32_update(0, data, len);
}

/* ---------- lectura de enteros little-endian ---------- */

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t rd64(const uint8_t *b)
{
    return (uint64_t)rd32(b) | ((uint64_t)rd32(b + 4) << 32);
}

static bool read_exact(map_capsule_t *cap, uint64_t off, uint8_t *out, size_t len)
{
    return cap->read_at(cap->ctx, off, out, len) == len;
}

/* ---------- apertura ---------- */

capsule_err_t map_capsule_open(map_capsule_t *cap, capsule_read_at_fn read_at,
                              void *ctx, uint64_t file_size)
{
    if (!cap || !read_at) {
        return CAPSULE_ERR_IO;
    }
    memset(cap, 0, sizeof(*cap));
    cap->read_at = read_at;
    cap->ctx = ctx;
    cap->file_size = file_size;

    if (file_size < CAPSULE_HEADER_SIZE) {
        return CAPSULE_ERR_TRUNCATED;
    }

    uint8_t hdr[CAPSULE_HEADER_SIZE];
    if (!read_exact(cap, 0, hdr, sizeof(hdr))) {
        return CAPSULE_ERR_IO;
    }
    if (memcmp(hdr, CAPSULE_MAGIC, CAPSULE_MAGIC_LEN) != 0) {
        return CAPSULE_ERR_MAGIC;
    }

    /* Distribución de la cabecera:
     *   0  magic (8)
     *   8  container_version u16
     *  10  reader_min_version u16
     *  12  flags u32
     *  16  manifest_offset u64
     *  24  manifest_len u32
     *  28  index_offset u64  (28..35)
     *  36  index_count u32
     *  40  index_entry_size u16
     *  42  reserved u16
     *  44  payload_region_offset u64
     *  52  payload_region_len u64
     *  60  header_crc32 u32   (sobre los primeros 60 bytes)
     */
    uint32_t declared_hdr_crc = rd32(&hdr[60]);
    if (capsule_crc32(hdr, 60) != declared_hdr_crc) {
        return CAPSULE_ERR_HEADER;
    }

    uint16_t reader_min = rd16(&hdr[10]);
    if (reader_min > CAPSULE_READER_VER) {
        /* La cápsula exige un lector más nuevo. Rechazar es lo correcto: interpretar
         * un formato que no se conoce daría límites de velocidad equivocados. */
        return CAPSULE_ERR_VERSION;
    }

    uint64_t manifest_off = rd64(&hdr[16]);
    uint32_t manifest_len = rd32(&hdr[24]);
    cap->index_offset = rd64(&hdr[28]);
    cap->index_count = rd32(&hdr[36]);
    uint16_t entry_size = rd16(&hdr[40]);
    cap->payload_region_offset = rd64(&hdr[44]);
    cap->payload_region_len = rd64(&hdr[52]);

    if (entry_size != CAPSULE_INDEX_ENTRY_SIZE) {
        return CAPSULE_ERR_HEADER;
    }
    /* Todo lo que la cabecera declara tiene que caber en el archivo. Sin esto, un
     * archivo truncado produciría lecturas fuera de rango. */
    if (manifest_off + manifest_len > file_size ||
        cap->index_offset + (uint64_t)cap->index_count * entry_size > file_size ||
        cap->payload_region_offset + cap->payload_region_len > file_size) {
        return CAPSULE_ERR_TRUNCATED;
    }

    /* Manifiesto. Distribución:
     *   0   region[64]
     *  64   content_version u32
     *  68   tile_size_e7 u32
     *  72   filename_scale u32
     *  76   cell_count u32
     *  80   created_unix u64
     *  88   index_crc32 u32
     *  92   signature[64]
     *  156  total
     */
    if (manifest_len < 156) {
        return CAPSULE_ERR_HEADER;
    }
    uint8_t man[156];
    if (!read_exact(cap, manifest_off, man, sizeof(man))) {
        return CAPSULE_ERR_IO;
    }
    memcpy(cap->manifest.region, man, CAPSULE_REGION_MAX);
    cap->manifest.region[CAPSULE_REGION_MAX - 1] = '\0';
    cap->manifest.content_version = rd32(&man[64]);
    cap->manifest.tile_size_e7 = rd32(&man[68]);
    cap->manifest.filename_scale = rd32(&man[72]);
    cap->manifest.cell_count = rd32(&man[76]);
    cap->manifest.created_unix = rd64(&man[80]);
    cap->manifest.index_crc32 = rd32(&man[88]);
    memcpy(cap->manifest.signature, &man[92], CAPSULE_SIG_MAX);

    if (cap->manifest.cell_count != cap->index_count) {
        return CAPSULE_ERR_HEADER;
    }

    /* CRC del índice completo, leído por páginas para no necesitar memoria por las
     * ~47 KiB que ocupa con la cobertura actual. */
    uint32_t crc = 0;
    uint64_t remaining = (uint64_t)cap->index_count * entry_size;
    uint64_t off = cap->index_offset;
    uint8_t page[512];
    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(page)) ? sizeof(page) : (size_t)remaining;
        if (!read_exact(cap, off, page, chunk)) {
            return CAPSULE_ERR_IO;
        }
        crc = capsule_crc32_update(crc, page, chunk);
        off += chunk;
        remaining -= chunk;
    }
    if (crc != cap->manifest.index_crc32) {
        return CAPSULE_ERR_INDEX_CRC;
    }

    cap->open = true;
    return CAPSULE_OK;
}

/* ---------- búsqueda ---------- */

static bool read_entry(map_capsule_t *cap, uint32_t idx, capsule_index_entry_t *out)
{
    uint8_t buf[CAPSULE_INDEX_ENTRY_SIZE];
    uint64_t off = cap->index_offset + (uint64_t)idx * CAPSULE_INDEX_ENTRY_SIZE;
    if (!read_exact(cap, off, buf, sizeof(buf))) {
        return false;
    }
    out->origin_lat_e7 = (int32_t)rd32(&buf[0]);
    out->origin_lon_e7 = (int32_t)rd32(&buf[4]);
    out->payload_offset = rd64(&buf[8]);
    out->payload_len = rd32(&buf[16]);
    out->payload_crc32 = rd32(&buf[20]);
    return true;
}

/* Orden del índice: por latitud y después por longitud. */
static int cmp_cell(int32_t alat, int32_t alon, int32_t blat, int32_t blon)
{
    if (alat != blat) {
        return (alat < blat) ? -1 : 1;
    }
    if (alon != blon) {
        return (alon < blon) ? -1 : 1;
    }
    return 0;
}

capsule_err_t map_capsule_find(map_capsule_t *cap, int32_t origin_lat_e7,
                              int32_t origin_lon_e7, capsule_index_entry_t *out)
{
    if (!cap || !cap->open || !out) {
        return CAPSULE_ERR_IO;
    }
    cap->lookups++;

    uint32_t lo = 0;
    uint32_t hi = cap->index_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        capsule_index_entry_t e;
        if (!read_entry(cap, mid, &e)) {
            return CAPSULE_ERR_IO;
        }
        int c = cmp_cell(e.origin_lat_e7, e.origin_lon_e7, origin_lat_e7, origin_lon_e7);
        if (c == 0) {
            /* La entrada tiene que caer dentro de la región de payloads declarada. */
            if (e.payload_offset < cap->payload_region_offset ||
                e.payload_offset + e.payload_len >
                    cap->payload_region_offset + cap->payload_region_len) {
                return CAPSULE_ERR_TRUNCATED;
            }
            *out = e;
            return CAPSULE_OK;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    cap->not_found++;
    return CAPSULE_ERR_NOT_FOUND;
}

capsule_err_t map_capsule_read_payload(map_capsule_t *cap,
                                      const capsule_index_entry_t *entry,
                                      uint8_t *out, size_t out_cap)
{
    if (!cap || !cap->open || !entry || !out) {
        return CAPSULE_ERR_IO;
    }
    if (entry->payload_len > out_cap) {
        return CAPSULE_ERR_IO;
    }
    if (!read_exact(cap, entry->payload_offset, out, entry->payload_len)) {
        return CAPSULE_ERR_IO;
    }
    if (capsule_crc32(out, entry->payload_len) != entry->payload_crc32) {
        /* Un payload corrupto no se entrega: darle bytes dañados al matcher
         * produciría un límite de velocidad inventado. */
        cap->crc_failures++;
        return CAPSULE_ERR_PAYLOAD_CRC;
    }
    return CAPSULE_OK;
}

capsule_err_t map_capsule_verify_all(map_capsule_t *cap, uint32_t *out_checked)
{
    if (!cap || !cap->open) {
        return CAPSULE_ERR_IO;
    }
    if (out_checked) {
        *out_checked = 0;
    }

    int32_t prev_lat = 0, prev_lon = 0;
    bool have_prev = false;

    for (uint32_t i = 0; i < cap->index_count; ++i) {
        capsule_index_entry_t e;
        if (!read_entry(cap, i, &e)) {
            return CAPSULE_ERR_IO;
        }
        /* El orden es parte del contrato: sin él la búsqueda binaria daría
         * resultados equivocados en lugar de fallar. */
        if (have_prev &&
            cmp_cell(prev_lat, prev_lon, e.origin_lat_e7, e.origin_lon_e7) >= 0) {
            return CAPSULE_ERR_HEADER;
        }
        prev_lat = e.origin_lat_e7;
        prev_lon = e.origin_lon_e7;
        have_prev = true;

        if (e.payload_offset < cap->payload_region_offset ||
            e.payload_offset + e.payload_len >
                cap->payload_region_offset + cap->payload_region_len) {
            return CAPSULE_ERR_TRUNCATED;
        }

        /* CRC del payload leyendo por páginas: no hace falta memoria del tamaño del
         * tile más grande. */
        uint32_t crc = 0;
        uint32_t remaining = e.payload_len;
        uint64_t off = e.payload_offset;
        uint8_t page[512];
        while (remaining > 0) {
            size_t chunk = (remaining > sizeof(page)) ? sizeof(page) : remaining;
            if (!read_exact(cap, off, page, chunk)) {
                return CAPSULE_ERR_IO;
            }
            crc = capsule_crc32_update(crc, page, chunk);
            off += chunk;
            remaining -= chunk;
        }
        if (crc != e.payload_crc32) {
            cap->crc_failures++;
            return CAPSULE_ERR_PAYLOAD_CRC;
        }
        if (out_checked) {
            (*out_checked)++;
        }
    }
    return CAPSULE_OK;
}

const char *capsule_err_name(capsule_err_t e)
{
    switch (e) {
    case CAPSULE_OK:               return "ok";
    case CAPSULE_ERR_IO:           return "io";
    case CAPSULE_ERR_MAGIC:        return "magic";
    case CAPSULE_ERR_VERSION:      return "version";
    case CAPSULE_ERR_HEADER:       return "cabecera";
    case CAPSULE_ERR_INDEX_CRC:    return "crc_indice";
    case CAPSULE_ERR_GEOMETRY:     return "geometria";
    case CAPSULE_ERR_TRUNCATED:    return "truncada";
    case CAPSULE_ERR_PAYLOAD_CRC:  return "crc_payload";
    case CAPSULE_ERR_NOT_FOUND:    return "no_encontrada";
    default:                       return "?";
    }
}
