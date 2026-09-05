/* Pruebas de robustez del matcher con una fuente sintética en memoria.
 *
 * Separadas del replay de equivalencia a propósito: acá se construyen tiles a mano
 * para llegar a casos que el dataset real no contiene —truncamiento en cada campo,
 * cantidad de segmentos mentida, nombre sin terminador— y que la matriz mínima de
 * fallas del plan exige cubrir en la fila «Mapas».
 *
 * Formato del tile v1, tal como lo emite python/extract_tiles.py:
 *   u32 n_segmentos
 *   por segmento: u16 n_puntos, n_puntos × (i32 lat_e7, i32 lon_e7),
 *                 u16 limite, u16 largo_nombre, bytes del nombre
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "map_match.h"
#include "tile_config.h"

/* ---------- constructor de tiles ---------- */

#define BUILD_MAX 4096

typedef struct {
    unsigned char buf[BUILD_MAX];
    size_t len;
} builder_t;

static void put_u16(builder_t *b, uint16_t v)
{
    assert(b->len + 2 <= BUILD_MAX);
    b->buf[b->len++] = (unsigned char)(v & 0xFF);
    b->buf[b->len++] = (unsigned char)(v >> 8);
}

static void put_u32(builder_t *b, uint32_t v)
{
    assert(b->len + 4 <= BUILD_MAX);
    for (int i = 0; i < 4; ++i) {
        b->buf[b->len++] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

static void put_i32(builder_t *b, int32_t v)
{
    put_u32(b, (uint32_t)v);
}

static void put_point(builder_t *b, double lat, double lon)
{
    put_i32(b, (int32_t)(lat * 1e7));
    put_i32(b, (int32_t)(lon * 1e7));
}

/* Un tile con un segmento recto que pasa por (lat, lon). */
static void build_one_segment(builder_t *b, double lat, double lon,
                             uint16_t limit, const char *name)
{
    memset(b, 0, sizeof(*b));
    put_u32(b, 1);                       /* un segmento */
    put_u16(b, 2);                       /* dos puntos  */
    put_point(b, lat - 0.0002, lon);
    put_point(b, lat + 0.0002, lon);
    put_u16(b, limit);
    uint16_t nlen = (uint16_t)(name ? strlen(name) : 0);
    put_u16(b, nlen);
    if (nlen) {
        memcpy(&b->buf[b->len], name, nlen);
        b->len += nlen;
    }
}

/* ---------- fuente sintética ---------- */

typedef struct {
    const unsigned char *bytes;
    size_t len;
    uint32_t fetches;
} mem_source_t;

static const uint8_t *mem_fetch(void *ctx, int32_t lat_e7, int32_t lon_e7, size_t *out_len)
{
    mem_source_t *ms = (mem_source_t *)ctx;
    (void)lat_e7;
    (void)lon_e7;
    ms->fetches++;
    /* El mismo contenido para cualquier tile pedido: alcanza para estas pruebas y
     * hace explícito que el matcher no depende de qué tile es. */
    *out_len = ms->len;
    return ms->len ? ms->bytes : NULL;
}

static const double LAT = -34.6000000;
static const double LON = -58.4000000;

static map_result_t query_with(const unsigned char *bytes, size_t len, uint32_t *out_fetches)
{
    mem_source_t ms = { bytes, len, 0 };
    map_tile_source_t src = { mem_fetch, &ms, 1 };
    map_match_state_t st;
    map_match_reset(&st);
    map_query_t q = { (float)LAT, (float)LON, 0.0f, 0.0f };
    map_result_t r;
    map_match_query(&st, &src, &q, &r);
    if (out_fetches) {
        *out_fetches = ms.fetches;
    }
    return r;
}

/* ---------- casos ---------- */

static void test_tile_completo_resuelve(void)
{
    builder_t b;
    build_one_segment(&b, LAT, LON, 40, "Calle Sintetica");
    map_result_t r = query_with(b.buf, b.len, NULL);

    assert(r.matched == true);
    assert(r.speed_kmh == 40);
    assert(strcmp(r.street, "Calle Sintetica") == 0);
    assert(r.limit_origin == MAP_LIMIT_NAMED_WAY);
    assert(r.tiles_truncated == 0);
}

static void test_truncado_en_cada_offset_no_rompe(void)
{
    /* El caso que la matriz de fallas pide: corte en cada campo. Ninguna longitud
     * puede provocar una lectura fuera de rango; ASan lo verifica. */
    builder_t b;
    build_one_segment(&b, LAT, LON, 40, "Calle Sintetica");

    for (size_t cut = 0; cut < b.len; ++cut) {
        map_result_t r = query_with(b.buf, cut, NULL);
        /* Con 0 bytes la fuente devuelve NULL: es un tile ausente, no truncado. */
        if (cut == 0) {
            assert(r.tiles_absent > 0);
            continue;
        }
        /* Con datos parciales no debe afirmar un límite salvo que haya alcanzado a
         * puntuar un segmento completo. Lo único obligatorio es que no rompa y que
         * el truncamiento quede contado. */
        if (!r.matched) {
            assert(r.tiles_truncated > 0);
        }
    }
}

static void test_truncamiento_queda_contado(void)
{
    builder_t b;
    build_one_segment(&b, LAT, LON, 40, "Calle Sintetica");
    /* Cortar en medio del nombre: el segmento no se puede cerrar. */
    map_result_t r = query_with(b.buf, b.len - 5, NULL);

    /* Antes esto se aceptaba en silencio: se dejaba de leer y el resultado parecía
     * completo. Ahora el truncamiento se informa, en los nueve tiles del barrido. */
    assert(r.tiles_truncated > 0);
    assert(r.matched == false);
}

static void test_cantidad_de_segmentos_mentida(void)
{
    /* Encabezado que declara 1000 segmentos y trae uno: el lector tiene que
     * detenerse por falta de bytes, no recorrer basura. */
    builder_t b;
    memset(&b, 0, sizeof(b));
    put_u32(&b, 1000);
    put_u16(&b, 2);
    put_point(&b, LAT - 0.0002, LON);
    put_point(&b, LAT + 0.0002, LON);
    put_u16(&b, 40);
    put_u16(&b, 4);
    memcpy(&b.buf[b.len], "Test", 4);
    b.len += 4;

    map_result_t r = query_with(b.buf, b.len, NULL);
    /* El primer segmento sí es válido, así que puede resolver; lo obligatorio es que
     * el resto se corte de forma contada. */
    assert(r.tiles_truncated > 0);
}

static void test_cantidad_de_puntos_mentida(void)
{
    builder_t b;
    memset(&b, 0, sizeof(b));
    put_u32(&b, 1);
    put_u16(&b, 5000);                /* dice 5000 puntos */
    put_point(&b, LAT, LON);          /* trae uno         */
    map_result_t r = query_with(b.buf, b.len, NULL);
    assert(r.matched == false);
    assert(r.tiles_truncated > 0);
}

static void test_nombre_vacio_da_via_anonima(void)
{
    /* Vía sin nombre con límite: el matcher informa el límite y marca el origen. */
    builder_t b;
    build_one_segment(&b, LAT, LON, 60, NULL);
    map_result_t r = query_with(b.buf, b.len, NULL);

    assert(r.matched == true);
    assert(r.speed_kmh == 60);
    assert(r.limit_origin == MAP_LIMIT_ANON_WAY);
    assert(r.street[0] == '\0');
}

static void test_via_sin_limite_no_afirma_limite(void)
{
    builder_t b;
    build_one_segment(&b, LAT, LON, 0, "Sin Limite");
    map_result_t r = query_with(b.buf, b.len, NULL);

    assert(r.matched == true);
    assert(r.speed_kmh == 0);
    assert(strcmp(r.street, "Sin Limite") == 0);
}

static void test_tile_vacio(void)
{
    builder_t b;
    memset(&b, 0, sizeof(b));
    put_u32(&b, 0);                   /* cero segmentos */
    map_result_t r = query_with(b.buf, b.len, NULL);
    assert(r.matched == false);
    assert(r.tiles_truncated == 0);   /* vacío no es truncado */
}

static void test_segmento_lejano_no_matchea(void)
{
    /* Más allá del corte de 50 m no se resuelve nada. */
    builder_t b;
    build_one_segment(&b, LAT + 0.01, LON + 0.01, 40, "Lejos");
    map_result_t r = query_with(b.buf, b.len, NULL);
    assert(r.matched == false);
}

static void test_barrido_de_nueve_tiles(void)
{
    /* Sin resultado cercano se consultan el tile base y sus ocho vecinos. */
    builder_t b;
    build_one_segment(&b, LAT + 0.01, LON + 0.01, 40, "Lejos");
    uint32_t fetches = 0;
    map_result_t r = query_with(b.buf, b.len, &fetches);
    (void)r;
    assert(fetches == 9);
    /* Y con un resultado muy cercano se saltan los vecinos. */
    build_one_segment(&b, LAT, LON, 40, "Encima");
    fetches = 0;
    r = query_with(b.buf, b.len, &fetches);
    assert(r.matched == true);
    assert(fetches == 1);
}

int main(void)
{
    test_tile_completo_resuelve();
    test_truncado_en_cada_offset_no_rompe();
    test_truncamiento_queda_contado();
    test_cantidad_de_segmentos_mentida();
    test_cantidad_de_puntos_mentida();
    test_nombre_vacio_da_via_anonima();
    test_via_sin_limite_no_afirma_limite();
    test_tile_vacio();
    test_segmento_lejano_no_matchea();
    test_barrido_de_nueve_tiles();
    printf("map_match robustez: tests passed\n");
    return 0;
}
