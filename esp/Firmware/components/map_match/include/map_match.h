/* Matcher de posición contra mapa: geometría pura, sin almacenamiento.
 *
 * Extraído en P04 desde `components/tile_reader/tile_reader.c`. **La matemática y
 * los parámetros son los mismos bytes**: umbrales, pesos de rumbo, bonus de
 * stickiness, orden de recorrido de vecinos y criterios de corte se movieron sin
 * tocar. La equivalencia se prueba contra el golden congelado en P00
 * (`host_tests/fixtures/tile_reader_baseline.golden`).
 *
 * Qué SÍ cambió, y por qué:
 *
 *  1. **La fuente de candidatos se inyecta.** Antes el matcher llamaba directo a
 *     `tile_cache_get()`, que abría archivos en la tarjeta. Ahora recibe un
 *     `map_tile_source_t`, así que el mismo C corre en el dispositivo contra la SD
 *     y en el host contra un directorio o contra memoria, sin `#ifdef`.
 *  2. **El estado es explícito y se puede resetear.** Antes vivía en `static` de
 *     función dentro de `get_speed_and_name_at()` —nombre y límite bloqueados,
 *     si había lock, si el último límite vino de una vía anónima— sin ninguna
 *     forma de limpiarlo. Eso hacía que el resultado de un fix dependiera de todos
 *     los anteriores y que dos replays en el mismo proceso se contaminaran.
 *  3. **El resultado es tipado**, con el origen del dato y la generación del
 *     dataset con que se calculó, para poder rechazar un resultado que pertenece a
 *     un mapa que ya no está montado.
 *
 * Sin ESP-IDF, sin memoria dinámica, sin locks.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Geometría del dataset v1 ----------
 * Conservada de la versión anterior. El generador (python/extract_tiles.py) emite
 * `tile_config.h` con TILE_SIZE_E7 y FILENAME_SCALE; acá sólo se necesita el
 * tamaño en grados para la aritmética de vecinos. */
#define TILE_SIZE   0.003
#define TILE_INV    (1.0 / TILE_SIZE)

#define MAX_POINTS         256                 // max points per polyline segment
#define MAX_STREET_NAME    128

#define EARTH_RADIUS       6371000.0f

// Distance cutoff limits
// for constraining the street limit from our location, in meters
#define URBAN_STREET_LIMIT      50.0f
#define MAX_STREET_DISTANCE     URBAN_STREET_LIMIT

/* Map-match tuning: heading + stickiness (meters / degrees / km/h). */
#define HEADING_WEIGHT_M_PER_DEG  0.20f  /* 45° ≈ +9 m score */
/* Switch margin: how much better a challenger must score to take over the
 * display. Dual carriageways can run 6-8 m apart (Acceso Norte / its colectora),
 * so 12 m was wider than the real separation and held the colectora on screen
 * for 26 s after merging onto the 130 lanes. Replayed against the tile set that
 * is actually on the SD card, 4 m drops that lag to zero -- the display follows
 * the merge on the same fix -- while 5 m still trailed by 8 s. Below 3 m the
 * limit starts chattering without getting any more accurate. */
#define STICK_BONUS_M             4.0f   /* prefer last locked street */
#define HEADING_MIN_SPEED_KMH     8.0f   /* COG unreliable when slower */
#define EARLY_EXIT_DIST_M         15.0f  /* only skip neighbors if this close */

/* ---------- Fuente de candidatos ---------- */

/**
 * @brief Entrega los bytes de un tile, o NULL si no existe.
 *
 * El puntero devuelto tiene que seguir siendo válido hasta la próxima llamada a
 * `fetch` sobre la misma fuente; el matcher decodifica los puntos en el lugar y no
 * copia. Quién es dueño de esa memoria y cuánto vive es problema de la fuente
 * (`map_store` en el dispositivo).
 *
 * @param ctx           contexto opaco de la fuente
 * @param origin_lat_e7 origen del tile, latitud en 1e7 fijo
 * @param origin_lon_e7 origen del tile, longitud en 1e7 fijo
 * @param out_len       recibe la longitud en bytes
 */
typedef const uint8_t *(*map_tile_fetch_fn)(void *ctx,
                                           int32_t origin_lat_e7,
                                           int32_t origin_lon_e7,
                                           size_t *out_len);

typedef struct {
    map_tile_fetch_fn fetch;
    void             *ctx;
    /** Generación del dataset. Viaja al resultado para poder rechazarlo. */
    uint64_t          generation;
} map_tile_source_t;

/* ---------- Estado del matcher ---------- */

/**
 * @brief Estado que persiste entre consultas.
 *
 * Antes eran `static` de función sin reset. Ahora es explícito: el llamador decide
 * cuándo empieza una corrida nueva, que es lo que permite replays deterministas y
 * lo que exige el cambio de tracker o de mapa.
 */
typedef struct {
    char locked_name[MAX_STREET_NAME];
    int  locked_speed;
    bool have_lock;
    /** Si el límite en pantalla vino de una vía sin nombre. */
    bool locked_anon;
} map_match_state_t;

/** Deja el estado como al arrancar. Obligatorio antes de cada replay. */
void map_match_reset(map_match_state_t *st);

/* ---------- Consulta y resultado ---------- */

typedef struct {
    float lat;
    float lon;
    float cog_deg;
    float speed_kmh;
} map_query_t;

/** De dónde salió el límite que se informa. */
typedef enum {
    MAP_LIMIT_NONE = 0,      /**< no se resolvió ningún límite               */
    MAP_LIMIT_NAMED_WAY,     /**< de la vía con nombre elegida               */
    MAP_LIMIT_ANON_WAY       /**< de una vía sin nombre más cercana          */
} map_limit_origin_t;

typedef struct {
    bool               matched;      /**< hubo resultado utilizable          */
    int                speed_kmh;    /**< límite informado, 0 si no hay      */
    char               street[MAX_STREET_NAME];
    map_limit_origin_t limit_origin;
    float              best_dist_m;  /**< distancia a la vía con nombre      */
    float              anon_dist_m;  /**< distancia a la vía anónima         */
    /** Generación del dataset con que se calculó. Permite rechazar un resultado
     *  que pertenece a un mapa que ya se desmontó o se reemplazó. */
    uint64_t           generation;
    /** Tiles consultados y ausentes en esta consulta, para diagnóstico. */
    uint32_t           tiles_scanned;
    uint32_t           tiles_absent;
} map_result_t;

/**
 * @brief Resuelve calle y límite para una posición.
 *
 * @param st     estado persistente; no puede ser NULL
 * @param src    fuente de candidatos; no puede ser NULL
 * @param q      consulta
 * @param out    resultado
 * @return el valor de `out->matched`, por comodidad
 */
bool map_match_query(map_match_state_t *st, const map_tile_source_t *src,
                    const map_query_t *q, map_result_t *out);

/** Texto corto del origen del límite, para logs y pruebas. */
const char *map_limit_origin_name(map_limit_origin_t o);

#ifdef __cplusplus
}
#endif
