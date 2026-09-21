/* Equivalencia cápsula contra directorio.
 *
 * La prueba que P07 necesita: el matcher alimentado desde la cápsula tiene que
 * producir **exactamente** los mismos resultados que alimentado desde el directorio
 * de tiles. Y no se compara contra el directorio: se compara contra el golden
 * congelado en P00, que es la referencia previa a toda esta reestructuración.
 *
 * Si esto pasa, entonces el contenedor no cambió los payloads —requisito textual del
 * plan, «empaquetar los payloads actuales sin cambiar su binario»— y el backend de
 * lectura por rangos es intercambiable con el de directorio.
 *
 * La cápsula la genera python/pack_capsule.py; la ruta llega por CAPSULE_PATH.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "map_match.h"
#include "map_capsule.h"
#include "tile_config.h"

#ifndef CAPSULE_PATH
#error "CAPSULE_PATH debe apuntar a la cápsula generada"
#endif
#ifndef GOLDEN_PATH
#define GOLDEN_PATH "fixtures/tile_reader_baseline.golden"
#endif

/* ---------- lector por rangos sobre un archivo ---------- */

typedef struct {
    FILE    *f;
    uint32_t reads;
    uint64_t bytes_read;
} file_reader_t;

static size_t file_read_at(void *ctx, uint64_t offset, uint8_t *out, size_t len)
{
    file_reader_t *fr = (file_reader_t *)ctx;
    if (fseeko(fr->f, (off_t)offset, SEEK_SET) != 0) {
        return 0;
    }
    size_t n = fread(out, 1, len, fr->f);
    fr->reads++;
    fr->bytes_read += n;
    return n;
}

/* ---------- fuente de candidatos del matcher, respaldada por la cápsula ---------- */

#define TILE_BUF_MAX (64 * 1024)

typedef struct {
    map_capsule_t *cap;
    uint8_t buf[TILE_BUF_MAX];
    uint32_t hits;
    uint32_t misses;
} capsule_source_t;

static const uint8_t *capsule_fetch(void *ctx, int32_t lat_e7, int32_t lon_e7,
                                   size_t *out_len)
{
    capsule_source_t *cs = (capsule_source_t *)ctx;
    *out_len = 0;

    capsule_index_entry_t entry;
    if (map_capsule_find(cs->cap, lat_e7, lon_e7, &entry) != CAPSULE_OK) {
        cs->misses++;
        return NULL;
    }
    if (map_capsule_read_payload(cs->cap, &entry, cs->buf, sizeof(cs->buf)) != CAPSULE_OK) {
        cs->misses++;
        return NULL;
    }
    cs->hits++;
    *out_len = entry.payload_len;
    return cs->buf;
}

/* ---------- la misma secuencia de fixes de P00 ---------- */

typedef struct { float lat, lon, cog_deg, speed_kmh; } fix_t;

static const fix_t FIXES[] = {
    { -34.4728243f, -58.4916759f, 302.0f, 40.0f },
    { -34.4810114f, -58.5093140f, 243.5f, 55.0f },
    { -34.4854611f, -58.4910674f, 149.6f, 30.0f },
    { -34.4909406f, -58.5493577f, 163.4f,  4.0f },
    { -34.4954183f, -58.5189738f, 241.7f, 25.0f },
    { -34.5023523f, -58.4918448f, 240.9f, 60.0f },
    { -34.5030632f, -58.5504492f, 221.4f, 12.0f },
    { -34.5092059f, -58.5117383f, 241.5f, 45.0f },
    { -34.5123804f, -58.5570657f,  43.6f, 20.0f },
    { -34.5203296f, -58.5003588f, 238.5f, 70.0f },
    { -34.5203296f, -58.5003588f, 238.5f, 70.0f },
    {   0.0000000f,   0.0000000f,   0.0f, 30.0f },
    { -34.4000000f, -58.3000000f,  90.0f, 80.0f },
};
#define NFIXES (sizeof(FIXES) / sizeof(FIXES[0]))

static void render(char *out, size_t out_len, size_t idx, const fix_t *f,
                  map_match_state_t *st, const map_tile_source_t *src)
{
    map_query_t q = { f->lat, f->lon, f->cog_deg, f->speed_kmh };
    map_result_t r;
    map_match_query(st, src, &q, &r);
    snprintf(out, out_len,
             "%02zu lat=%.7f lon=%.7f cog=%.1f v=%.1f -> match=%d speed=%d street=\"%s\"",
             idx, (double)f->lat, (double)f->lon, (double)f->cog_deg,
             (double)f->speed_kmh, r.matched ? 1 : 0,
             r.matched ? r.speed_kmh : -1, r.matched ? r.street : "");
}

static int compare_against_golden(char lines[NFIXES][512])
{
    FILE *f = fopen(GOLDEN_PATH, "r");
    if (!f) {
        fprintf(stderr, "falta el golden %s\n", GOLDEN_PATH);
        return 2;
    }
    int failures = 0;
    size_t idx = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n == 0 || buf[0] == '#' || strncmp(buf, "cache ", 6) == 0) {
            continue;
        }
        if (idx >= NFIXES) {
            fprintf(stderr, "el golden tiene más filas que la lista de fixes\n");
            failures++;
            break;
        }
        if (strcmp(buf, lines[idx]) != 0) {
            fprintf(stderr, "DIFERENCIA en la fila %zu\n  golden (directorio, P00): %s\n"
                            "  capsula (ahora):          %s\n", idx, buf, lines[idx]);
            failures++;
        }
        idx++;
    }
    fclose(f);
    if (idx != NFIXES) {
        fprintf(stderr, "el golden tiene %zu filas, se esperaban %zu\n", idx, NFIXES);
        failures++;
    }
    return failures;
}

/* ---------- pruebas del contenedor ---------- */

static void test_manifiesto_declara_la_geometria_real(map_capsule_t *cap)
{
    /* Si la cápsula declarara otra geometría que la que compila el matcher, las
     * celdas se buscarían con un tamaño equivocado y el resultado sería silenciosa
     * y sistemáticamente incorrecto. */
    assert(cap->manifest.tile_size_e7 == TILE_SIZE_E7);
    assert(cap->manifest.filename_scale == FILENAME_SCALE);
    assert(cap->manifest.cell_count == cap->index_count);
    assert(cap->manifest.cell_count == 1961);
}

static void test_firma_esta_en_cero(map_capsule_t *cap)
{
    /* La firma se declara reservada para P10. Que esté en cero es lo esperado, y
     * dejarlo aseverado evita que alguien la crea verificada. */
    for (size_t i = 0; i < CAPSULE_SIG_MAX; ++i) {
        assert(cap->manifest.signature[i] == 0);
    }
}

static void test_celda_inexistente(map_capsule_t *cap)
{
    capsule_index_entry_t e;
    /* Medio Atlántico: no está en la cobertura. */
    assert(map_capsule_find(cap, 0, 0, &e) == CAPSULE_ERR_NOT_FOUND);
}

static void test_verificacion_completa(map_capsule_t *cap)
{
    /* Es lo que hay que correr antes de marcar READY una candidata: el plan exige
     * «verificar completamente y reabrir con lector real antes de marcar READY». */
    uint32_t checked = 0;
    capsule_err_t err = map_capsule_verify_all(cap, &checked);
    assert(err == CAPSULE_OK);
    assert(checked == cap->index_count);
}

int main(void)
{
    file_reader_t fr;
    memset(&fr, 0, sizeof(fr));
    fr.f = fopen(CAPSULE_PATH, "rb");
    if (!fr.f) {
        fprintf(stderr, "no se pudo abrir la cápsula %s\n"
                        "generarla con: python3 python/pack_capsule.py tiles/ %s\n",
                CAPSULE_PATH, CAPSULE_PATH);
        return 2;
    }
    fseeko(fr.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(fr.f);

    static map_capsule_t cap;
    capsule_err_t err = map_capsule_open(&cap, file_read_at, &fr, size);
    if (err != CAPSULE_OK) {
        fprintf(stderr, "no se pudo abrir la cápsula: %s\n", capsule_err_name(err));
        return 1;
    }

    test_manifiesto_declara_la_geometria_real(&cap);
    test_firma_esta_en_cero(&cap);
    test_celda_inexistente(&cap);

    /* Equivalencia contra el golden de P00. */
    static capsule_source_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.cap = &cap;
    map_tile_source_t src = { capsule_fetch, &cs, 1 };

    static char lines[NFIXES][512];
    map_match_state_t st;
    map_match_reset(&st);
    for (size_t i = 0; i < NFIXES; ++i) {
        render(lines[i], sizeof(lines[i]), i, &FIXES[i], &st, &src);
    }

    int failures = compare_against_golden(lines);
    if (failures) {
        fprintf(stderr, "\nEQUIVALENCIA CAPSULA FALLIDA (%d filas distintas).\n"
                        "El golden es la referencia de P00, leída del directorio de\n"
                        "tiles. Una diferencia significa que el contenedor cambió los\n"
                        "payloads o que el índice resuelve celdas equivocadas.\n",
                failures);
        return 1;
    }

    test_verificacion_completa(&cap);

    printf("capsula: equivalencia con el directorio confirmada sobre %zu fixes\n",
           NFIXES);
    printf("  celdas=%" PRIu32 " aciertos=%" PRIu32 " ausencias=%" PRIu32
           " lecturas=%" PRIu32 " bytes=%" PRIu64 "\n",
           cap.index_count, cs.hits, cs.misses, fr.reads, fr.bytes_read);
    fclose(fr.f);
    return 0;
}
