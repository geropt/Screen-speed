/* Replay de trayectorias reales contra el sistema de tiles.
 *
 * Esto es el corpus que P04 pedía y no existía: en lugar de 13 fixes derivados del
 * propio dataset, recorre las trayectorias completas de capturas de campo y evalúa qué
 * contesta el matcher fix por fix.
 *
 * Qué responde, con números:
 *  - cuántos fixes resuelven calle y cuántos resuelven límite;
 *  - cuán estable es el resultado: una calle real tiene que sostenerse muchos fixes
 *    seguidos, no cambiar en cada uno. El «flapping» es la falla que más se nota en
 *    pantalla y la que ningún test sintético detecta;
 *  - qué calles aparecen, para poder mirar si tienen sentido en la ruta.
 *
 * El resumen se compara contra un golden por captura. No es una prueba de exactitud
 * —no hay verdad de referencia de en qué calle estaba el vehículo—; es una prueba de
 * **no regresión**: si un cambio en el matcher mueve estos números, se ve.
 *
 * El parseo de los campos de RMC lo hace este archivo, independiente del parser del
 * firmware. Eso es a propósito: dos implementaciones distintas que coinciden en las
 * coordenadas son evidencia; una que se verifica consigo misma no lo es.
 */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmea_framer.h"
#include "map_match.h"
#include "tile_config.h"

#ifndef DATASET_DIR
#error "DATASET_DIR debe apuntar al directorio que contiene tiles/"
#endif

/* ---------- fuente de tiles sobre el directorio ---------- */

#define TILE_BUF_MAX (64 * 1024)

typedef struct {
    unsigned char buf[TILE_BUF_MAX];
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
    snprintf(path, sizeof(path),
             DATASET_DIR "/tiles/%" PRId32 "/tile_%" PRId32 "_%" PRId32 ".bin",
             filename_coord_of(lat_e7), filename_coord_of(lat_e7),
             filename_coord_of(lon_e7));
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

/* ---------- parseo independiente de RMC ---------- */

typedef struct {
    float lat, lon, speed_kmh, cog;
} fix_t;

#define MAX_FIXES 2048
static fix_t g_fixes[MAX_FIXES];
static size_t g_nfixes;
static uint32_t g_rmc_seen, g_rmc_invalid, g_rmc_unparsable;

/* Grados y minutos: ddmm.mmmm / dddmm.mmmm */
static bool degmin(const char *v, char hemi, int deg_digits, float *out)
{
    if (!v || strlen(v) < (size_t)deg_digits + 3) {
        return false;
    }
    char dbuf[4] = {0};
    memcpy(dbuf, v, (size_t)deg_digits);
    float deg = (float)atof(dbuf);
    float min = (float)atof(v + deg_digits);
    float x = deg + min / 60.0f;
    if (hemi == 'S' || hemi == 'W') {
        x = -x;
    }
    *out = x;
    return true;
}

static void on_sentence(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    (void)len;
    if (!strstr(s, "RMC")) {
        return;
    }
    g_rmc_seen++;

    /* Copiar y trocear por comas. */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    char *fields[16] = {0};
    int nf = 0;
    char *p = buf;
    fields[nf++] = p;
    while (*p && nf < 16) {
        if (*p == ',') {
            *p = '\0';
            fields[nf++] = p + 1;
        }
        p++;
    }
    if (nf < 9) {
        g_rmc_unparsable++;
        return;
    }
    /* fields[0]=$GNRMC 1=hora 2=status 3=lat 4=N/S 5=lon 6=E/W 7=vel 8=rumbo */
    if (fields[2][0] != 'A') {
        /* Receptor sin fix: el firmware tampoco lo publica. */
        g_rmc_invalid++;
        return;
    }
    float lat, lon;
    if (!degmin(fields[3], fields[4][0], 2, &lat) ||
        !degmin(fields[5], fields[6][0], 3, &lon)) {
        g_rmc_unparsable++;
        return;
    }
    if (g_nfixes >= MAX_FIXES) {
        return;
    }
    g_fixes[g_nfixes].lat = lat;
    g_fixes[g_nfixes].lon = lon;
    /* La velocidad de RMC viene en nudos. */
    g_fixes[g_nfixes].speed_kmh = (float)atof(fields[7]) * 1.852f;
    g_fixes[g_nfixes].cog = (float)atof(fields[8]);
    g_nfixes++;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: %s <captura.bin> [--update <golden>]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "no se pudo abrir %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        return 2;
    }
    fclose(f);

    /* Framing con el componente compartido, igual que en el dispositivo. */
    nmea_framer_t framer;
    nmea_framer_init(&framer, on_sentence, NULL);
    /* Trozos de 64 bytes: el corte no coincide con las fronteras de sentencia. */
    for (long off = 0; off < size; off += 64) {
        size_t n = (size - off < 64) ? (size_t)(size - off) : 64;
        nmea_framer_feed(&framer, data + off, n);
    }
    free(data);

    /* Recorrer la trayectoria. */
    static dir_source_t ds;
    memset(&ds, 0, sizeof(ds));
    map_tile_source_t src = { dir_fetch, &ds, 1 };
    map_match_state_t st;
    map_match_reset(&st);

    uint32_t matched = 0, with_limit = 0, street_changes = 0, limit_changes = 0;
    uint32_t anon = 0;
    char prev_street[MAX_STREET_NAME] = "";
    int prev_limit = -1;
    uint32_t tiles_scanned = 0, tiles_absent = 0, truncated = 0;

    /* Conteo por calle: permite que una persona que conoce la ruta verifique si los
     * nombres tienen sentido. Es la única forma de detectar un matcher que resuelve
     * mucho y resuelve mal. */
    struct { char name[MAX_STREET_NAME]; uint32_t n; int limit; } tally[64];
    size_t ntally = 0;

    for (size_t i = 0; i < g_nfixes; ++i) {
        map_query_t q = { g_fixes[i].lat, g_fixes[i].lon,
                          g_fixes[i].cog, g_fixes[i].speed_kmh };
        map_result_t r;
        map_match_query(&st, &src, &q, &r);
        tiles_scanned += r.tiles_scanned;
        tiles_absent += r.tiles_absent;
        truncated += r.tiles_truncated;

        if (!r.matched) {
            continue;
        }
        matched++;
        if (r.speed_kmh > 0) {
            with_limit++;
        }
        if (r.limit_origin == MAP_LIMIT_ANON_WAY) {
            anon++;
        }
        if (r.street[0]) {
            size_t k = 0;
            for (; k < ntally; ++k) {
                if (strcmp(tally[k].name, r.street) == 0) {
                    tally[k].n++;
                    break;
                }
            }
            if (k == ntally && ntally < 64) {
                snprintf(tally[ntally].name, MAX_STREET_NAME, "%s", r.street);
                tally[ntally].n = 1;
                tally[ntally].limit = r.speed_kmh;
                ntally++;
            }
        }
        if (strcmp(prev_street, r.street) != 0) {
            if (prev_street[0]) {
                street_changes++;
            }
            snprintf(prev_street, sizeof(prev_street), "%s", r.street);
        }
        if (prev_limit != r.speed_kmh) {
            if (prev_limit >= 0) {
                limit_changes++;
            }
            prev_limit = r.speed_kmh;
        }
    }

    /* El resumen es la unidad de comparación: números agregados, no cada fix, para que
     * el golden no sea ilegible ni frágil ante una diferencia de un solo fix. */
    /* Sólo el nombre base: la ruta cambia según desde dónde se invoque, y meterla en
     * el golden lo haría fallar por algo que no tiene nada que ver con el matcher. */
    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];

    char summary[1024];
    int len = snprintf(summary, sizeof(summary),
        "captura=%s\n"
        "rmc_vistas=%u rmc_sin_fix=%u rmc_no_parseables=%u fixes=%zu\n"
        "resueltos=%u sin_resolver=%zu con_limite=%u de_via_anonima=%u\n"
        "cambios_de_calle=%u cambios_de_limite=%u\n"
        "tiles_consultados=%u ausentes=%u truncados=%u\n",
        base, g_rmc_seen, g_rmc_invalid, g_rmc_unparsable, g_nfixes,
        matched, g_nfixes - matched, with_limit, anon,
        street_changes, limit_changes,
        tiles_scanned, tiles_absent, truncated);

    /* Las cinco calles con más fixes, ordenadas. */
    for (int top = 0; top < 5; ++top) {
        size_t best = ntally;
        for (size_t k = 0; k < ntally; ++k) {
            if (tally[k].n == 0) continue;
            if (best == ntally || tally[k].n > tally[best].n) best = k;
        }
        if (best == ntally) break;
        len += snprintf(summary + len, sizeof(summary) - (size_t)len,
                        "calle: %-40s %4u fixes  limite %d\n",
                        tally[best].name, tally[best].n, tally[best].limit);
        tally[best].n = 0;
    }
    len += snprintf(summary + len, sizeof(summary) - (size_t)len,
                    "calles_distintas=%zu\n", ntally);

    const char *golden = NULL;
    bool update = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--update") == 0 && i + 1 < argc) {
            update = true;
            golden = argv[++i];
        } else if (strcmp(argv[i], "--golden") == 0 && i + 1 < argc) {
            golden = argv[++i];
        }
    }

    fputs(summary, stdout);

    if (!golden) {
        return 0;
    }
    if (update) {
        FILE *g = fopen(golden, "w");
        if (!g) {
            fprintf(stderr, "no se pudo escribir %s\n", golden);
            return 2;
        }
        fputs(summary, g);
        fclose(g);
        printf("golden actualizado: %s\n", golden);
        return 0;
    }

    FILE *g = fopen(golden, "r");
    if (!g) {
        fprintf(stderr, "falta el golden %s (generar con --update)\n", golden);
        return 2;
    }
    char expected[1024] = {0};
    size_t n = fread(expected, 1, sizeof(expected) - 1, g);
    expected[n] = '\0';
    fclose(g);

    if (strcmp(expected, summary) != 0) {
        fprintf(stderr, "\nDIFERENCIA contra el golden de trayectoria.\n"
                        "esperado:\n%s\nobtenido:\n%s\n"
                        "Un cambio acá significa que el matcher contesta distinto sobre\n"
                        "trayectorias reales. Si es intencional, actualizar el golden en\n"
                        "el mismo commit que lo explica.\n", expected, summary);
        return 1;
    }
    printf("  coincide con el golden\n");
    return 0;
}
