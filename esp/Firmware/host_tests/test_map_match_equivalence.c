/* Replay de equivalencia de P04.
 *
 * Corre el matcher **extraído** (`components/map_match`) sobre la misma secuencia de
 * fixes y el mismo dataset que la caracterización de P00, y compara contra el mismo
 * golden congelado: `fixtures/tile_reader_baseline.golden`.
 *
 * Ese archivo se generó ANTES de la extracción, con el código viejo y su estado
 * `static` de función. Si este programa lo reproduce línea por línea, la extracción
 * preservó el comportamiento. Es la puerta que el plan pide: «no cerrar una
 * extracción aceptando diferencias bajo el argumento de que ahora parece mejor».
 *
 * La fuente de candidatos acá lee el directorio del dataset directamente, sin la
 * caché del dispositivo, lo que además demuestra que el matcher quedó desacoplado
 * del almacenamiento: mismo C, otra fuente, mismo resultado.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "map_match.h"
#include "tile_config.h"

#ifndef GOLDEN_PATH
#define GOLDEN_PATH "fixtures/tile_reader_baseline.golden"
#endif
#ifndef DATASET_DIR
#error "DATASET_DIR debe apuntar al directorio que contiene tiles/"
#endif

/* ---------- fuente de candidatos sobre el directorio del dataset ----------
 *
 * Reproduce el contrato de ownership de map_store: el puntero devuelto vale hasta
 * la próxima llamada. Un solo buffer, reusado, sin caché: así el resultado no puede
 * depender de aciertos de caché.
 */
#define DIR_SOURCE_MAX_TILE (64 * 1024)

typedef struct {
    unsigned char buf[DIR_SOURCE_MAX_TILE];
    uint32_t reads;
    uint32_t absent;
} dir_source_t;

static int32_t filename_coord_of(int32_t origin_e7)
{
    return origin_e7 / FILENAME_SCALE;
}

static const uint8_t *dir_fetch(void *ctx, int32_t lat_e7, int32_t lon_e7, size_t *out_len)
{
    dir_source_t *ds = (dir_source_t *)ctx;
    char path[512];
    snprintf(path, sizeof(path), DATASET_DIR "/tiles/%" PRId32 "/tile_%" PRId32 "_%" PRId32 ".bin",
             filename_coord_of(lat_e7), filename_coord_of(lat_e7), filename_coord_of(lon_e7));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ds->absent++;
        *out_len = 0;
        return NULL;
    }
    size_t n = fread(ds->buf, 1, sizeof(ds->buf), f);
    fclose(f);
    ds->reads++;
    *out_len = n;
    return ds->buf;
}

/* ---------- la misma secuencia de fixes que P00 ----------
 * Copiada de host_tests/test_tile_reader_baseline.c sin cambiar un valor: si esta
 * lista cambia, el golden deja de ser comparable.
 */
typedef struct {
    float lat, lon, cog_deg, speed_kmh;
} fix_t;

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

/* Formato idéntico al de la caracterización de P00. */
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
             r.matched ? r.speed_kmh : -1,
             r.matched ? r.street : "");
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
        if (n == 0 || buf[0] == '#') {
            continue;
        }
        /* La última línea del golden son los contadores de la caché del
         * dispositivo, que no aplican a esta fuente. Se ignora explícitamente. */
        if (strncmp(buf, "cache ", 6) == 0) {
            continue;
        }
        if (idx >= NFIXES) {
            fprintf(stderr, "el golden tiene más filas de fix que la lista\n");
            failures++;
            break;
        }
        if (strcmp(buf, lines[idx]) != 0) {
            fprintf(stderr, "DIFERENCIA en la fila %zu\n  golden (antes): %s\n"
                            "  extraido (ahora): %s\n", idx, buf, lines[idx]);
            failures++;
        }
        idx++;
    }
    fclose(f);
    if (idx != NFIXES) {
        fprintf(stderr, "el golden tiene %zu filas de fix, se esperaban %zu\n", idx, NFIXES);
        failures++;
    }
    return failures;
}

/* ---------- pruebas del estado explícito ---------- */

static void test_reset_hace_determinista_el_replay(dir_source_t *ds,
                                                  const map_tile_source_t *src)
{
    /* La razón de ser del reset: antes el estado era `static` de función y dos
     * corridas en el mismo proceso se contaminaban. Con reset, dos replays de la
     * misma secuencia tienen que dar exactamente lo mismo. */
    (void)ds;
    char a[NFIXES][512];
    char b[NFIXES][512];

    map_match_state_t st;
    map_match_reset(&st);
    for (size_t i = 0; i < NFIXES; ++i) {
        render(a[i], sizeof(a[i]), i, &FIXES[i], &st, src);
    }
    map_match_reset(&st);
    for (size_t i = 0; i < NFIXES; ++i) {
        render(b[i], sizeof(b[i]), i, &FIXES[i], &st, src);
    }
    for (size_t i = 0; i < NFIXES; ++i) {
        assert(strcmp(a[i], b[i]) == 0);
    }

    /* Y sin reset, la segunda corrida NO tiene por qué coincidir: el lock de calle
     * sobrevive. Se comprueba que el estado realmente lleva información, para que
     * el reset no sea decorativo. */
    map_match_state_t st2;
    map_match_reset(&st2);
    map_query_t q = { FIXES[0].lat, FIXES[0].lon, FIXES[0].cog_deg, FIXES[0].speed_kmh };
    map_result_t r;
    map_match_query(&st2, src, &q, &r);
    assert(r.matched);
    assert(st2.have_lock == true);
    assert(st2.locked_name[0] != '\0');
    map_match_reset(&st2);
    assert(st2.have_lock == false);
    assert(st2.locked_name[0] == '\0');
}

static void test_generacion_viaja_al_resultado(const map_tile_source_t *src)
{
    map_tile_source_t s2 = *src;
    s2.generation = 7;
    map_match_state_t st;
    map_match_reset(&st);
    map_query_t q = { FIXES[0].lat, FIXES[0].lon, FIXES[0].cog_deg, FIXES[0].speed_kmh };
    map_result_t r;
    map_match_query(&st, &s2, &q, &r);
    /* Permite rechazar un resultado calculado con un mapa que ya no está. */
    assert(r.generation == 7);
}

static void test_origen_del_limite(const map_tile_source_t *src)
{
    map_match_state_t st;
    map_match_reset(&st);
    map_query_t q = { FIXES[0].lat, FIXES[0].lon, FIXES[0].cog_deg, FIXES[0].speed_kmh };
    map_result_t r;
    map_match_query(&st, src, &q, &r);
    assert(r.matched);
    /* El resultado dice de dónde salió el límite, que antes no se informaba. */
    assert(r.limit_origin == MAP_LIMIT_NAMED_WAY || r.limit_origin == MAP_LIMIT_ANON_WAY);
    assert(r.tiles_scanned >= 1);
}

static void test_null_island_no_toca_la_fuente(void)
{
    dir_source_t ds;
    memset(&ds, 0, sizeof(ds));
    map_tile_source_t src = { dir_fetch, &ds, 1 };
    map_match_state_t st;
    map_match_reset(&st);
    map_query_t q = { 0.0f, 0.0f, 0.0f, 30.0f };
    map_result_t r;
    assert(map_match_query(&st, &src, &q, &r) == false);
    assert(r.limit_origin == MAP_LIMIT_NONE);
    /* Ni una lectura: el rechazo ocurre antes de mirar el almacenamiento. */
    assert(ds.reads == 0);
    assert(ds.absent == 0);
}

static void test_punteros_nulos(const map_tile_source_t *src)
{
    map_result_t r;
    map_match_state_t st;
    map_match_reset(&st);
    map_query_t q = { -34.5f, -58.5f, 0.0f, 10.0f };
    assert(map_match_query(NULL, src, &q, &r) == false);
    assert(map_match_query(&st, NULL, &q, &r) == false);
    assert(map_match_query(&st, src, NULL, &r) == false);
    assert(map_match_query(&st, src, &q, NULL) == false);
    map_match_reset(NULL);
    assert(strcmp(map_limit_origin_name(MAP_LIMIT_ANON_WAY), "via_anonima") == 0);
}

int main(void)
{
    static dir_source_t ds;
    memset(&ds, 0, sizeof(ds));
    map_tile_source_t src = { dir_fetch, &ds, 1 };

    static char lines[NFIXES][512];
    map_match_state_t st;
    map_match_reset(&st);
    for (size_t i = 0; i < NFIXES; ++i) {
        render(lines[i], sizeof(lines[i]), i, &FIXES[i], &st, &src);
    }

    int failures = compare_against_golden(lines);
    if (failures) {
        fprintf(stderr, "\nEQUIVALENCIA FALLIDA (%d filas distintas).\n"
                        "El golden es la referencia de P00, previa a la extracción.\n"
                        "Una diferencia acá significa que la extracción cambió el\n"
                        "comportamiento: hay que corregirla, no actualizar el golden.\n",
                failures);
        return 1;
    }

    test_reset_hace_determinista_el_replay(&ds, &src);
    test_generacion_viaja_al_resultado(&src);
    test_origen_del_limite(&src);
    test_null_island_no_toca_la_fuente();
    test_punteros_nulos(&src);

    printf("map_match: equivalencia v1 confirmada sobre %zu fixes (%u lecturas, %u ausentes)\n",
           NFIXES, (unsigned)ds.reads, (unsigned)ds.absent);
    return 0;
}
