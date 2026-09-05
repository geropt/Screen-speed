#include "map_match.h"
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "tile_config.h"

/* Sin esp_log: este componente compila igual en el host. Lo que antes se logueaba
 * por tile ausente ahora se cuenta en el resultado (`tiles_absent`), que además es
 * observable desde una prueba. */

/* ---------- Binary reader over a cached tile ----------
 * Tiles now arrive as one PSRAM buffer instead of a FILE *, so bounds checking
 * is ours to do. Every read goes through the cursor; once it runs past the end
 * `ok` latches false and the caller stops. Values stay byte-assembled because
 * the records are packed and nothing guarantees alignment. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool ok;
} tile_rd_t;

static inline bool rd_take(tile_rd_t *r, size_t n)
{
    if (!r->ok || (size_t)(r->end - r->p) < n) {
        r->ok = false;
        return false;
    }
    return true;
}

static uint32_t rd_u32(tile_rd_t *r)
{
    if (!rd_take(r, 4)) return 0;
    const uint8_t *b = r->p;
    r->p += 4;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd_u16(tile_rd_t *r)
{
    if (!rd_take(r, 2)) return 0;
    const uint8_t *b = r->p;
    r->p += 2;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

/* Borrow n bytes in place and advance; NULL when the tile is truncated. */
static const uint8_t *rd_block(tile_rd_t *r, size_t n)
{
    if (!rd_take(r, n)) return NULL;
    const uint8_t *b = r->p;
    r->p += n;
    return b;
}

/* Points are stored as pairs of little-endian int32 in 1e7 fixed point. */
static inline float pt_at(const uint8_t *pts, uint16_t idx, int which)
{
    const uint8_t *b = pts + (size_t)idx * 8 + (which ? 4 : 0);
    int32_t v = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
    return v / 1e7f;
}
#define PT_LAT(pts, i) pt_at((pts), (i), 0)
#define PT_LON(pts, i) pt_at((pts), (i), 1)

// ------------------ tile index ------------------
static inline int32_t deg_to_e7(double deg) {
    return (int32_t)floor(deg * 1e7);
}

static inline int32_t floor_div(int32_t a, int32_t b) {
    int32_t q = a / b;
    int32_t r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0)))
        q--;
    return q;
}

static inline int32_t tile_origin_e7(int32_t coord_e7) {
    return floor_div(coord_e7, TILE_SIZE_E7) * TILE_SIZE_E7;
}

/* `filename_coord()` se fue con el almacenamiento: construir nombres de archivo es
 * de map_store, no de la geometría. */

static float haversine(float lat1, float lon1, float lat2, float lon2)
{
    float dlat = (lat2 - lat1) * (float)M_PI / 180.0f;
    float dlon = (lon2 - lon1) * (float)M_PI / 180.0f;

    lat1 *= (float)M_PI / 180.0f;
    lat2 *= (float)M_PI / 180.0f;

    float a = sinf(dlat/2)*sinf(dlat/2) +
              cosf(lat1)*cosf(lat2) * sinf(dlon/2)*sinf(dlon/2);

    float c = 2.0f * atanf(sqrtf(a) / sqrtf(1.0f - a));

    return EARTH_RADIUS * c;
}

/**
 * Accurate distance from a point P to segment AB using:
 * 1. Euclidean projection (fast)
 * 2. Haversine distance for actual meters
 */
static float distance_point_to_segment_haversine(float plat, float plon,
                                                 float alat, float alon,
                                                 float blat, float blon)
{
    float ax = alat, ay = alon;
    float bx = blat, by = blon;
    float px = plat, py = plon;

    float vx = bx - ax;
    float vy = by - ay;

    float wx = px - ax;
    float wy = py - ay;

    float c1 = vx*wx + vy*wy;
    if (c1 <= 0)
        return haversine(px, py, ax, ay);

    float c2 = vx*vx + vy*vy;
    if (c2 <= c1)
        return haversine(px, py, bx, by);

    float t = c1 / c2;  // projection factor

    float proj_lat = ax + t * vx;
    float proj_lon = ay + t * vy;

    return haversine(px, py, proj_lat, proj_lon);
}

/* Smallest angle between COG and segment bearing, treating reverse as same road. */
static float heading_err_deg(float cog_deg, float alat, float alon, float blat, float blon)
{
    float dlat = blat - alat;
    float dlon = (blon - alon) * cosf(alat * (float)M_PI / 180.0f);
    if (dlat == 0.0f && dlon == 0.0f)
        return 90.0f;

    float brng = atan2f(dlon, dlat) * (180.0f / (float)M_PI);
    if (brng < 0.0f)
        brng += 360.0f;

    float d = fabsf(cog_deg - brng);
    if (d > 180.0f)
        d = 360.0f - d;
    if (d > 90.0f)
        d = 180.0f - d;
    return d;
}

// --------------------------------------------------------
//         MAIN FUNCTION (called by the rest of app)
// --------------------------------------------------------
static bool scan_tile_for_match(const map_tile_source_t *src, map_result_t *stats,
                                int32_t tx, int32_t ty, float lat, float lon,
                                float cog_deg, bool use_heading,
                                const char *preferred_name, int preferred_speed,
                                float *outBestScore, float *outBestDist,
                                int *outBestSpeed,
                                char *outBestName, size_t bestNameSize,
                                bool *outTileHasData,
                                float *outAnonDist, int *outAnonSpeed)
{
    *outTileHasData = false;

    size_t tileLen = 0;
    const uint8_t *tileBuf = src->fetch(src->ctx, tx, ty, &tileLen);
    stats->tiles_scanned++;
    if (!tileBuf) {
        stats->tiles_absent++;
        /* Expected at the edge of coverage: the 8 neighbors of a border tile
         * simply do not exist. La fuente recuerda la ausencia, así que después de
         * la primera consulta esto no cuesta nada. */
        return false;  // tile does not exist
    }

    tile_rd_t rd = { tileBuf, tileBuf + tileLen, true };
    uint32_t segCount = rd_u32(&rd);
    if (!rd.ok) {
        /* El archivo no alcanza ni para el contador de segmentos. Antes esto se
         * confundía con un tile vacío: segCount quedaba en 0, el bucle no corría y
         * el resultado parecía «acá no hay calles» en lugar de «este archivo está
         * dañado». Son cosas distintas y ahora se distinguen. */
        stats->tiles_truncated++;
    }
    float localBestScore = 1e12f;
    float localBestDist = 1e12f;
    int   localBestSpeed = 0;
    char  localBestName[128] = "";
    /* Closest way that carries a posted limit but no name -- OSM splits
     * carriageways and the resulting segments often keep only `ref`. At the
     * General Paz / Acceso Norte junction the roadway actually being driven sits
     * 2.5 m away with maxspeed=100 and no name, while the nearest *named* way is
     * 35 m off, so the name filter alone was throwing away the right answer. */
    float localAnonDist = 1e12f;
    int   localAnonSpeed = 0;
    const float max_hdg_penalty = use_heading ? (HEADING_WEIGHT_M_PER_DEG * 90.0f) : 0.0f;

    for (uint32_t s = 0; s < segCount; s++) {

        uint16_t numPoints = rd_u16(&rd);

        float segBestScore = 1e12f;
        float segBestDist = 1e12f;

        /* The points stay where they are in the cached buffer and get decoded on
         * demand. That removes the two per-segment mallocs the file-based reader
         * needed -- and with them the whole out-of-memory path. */
        const uint8_t *pts = rd_block(&rd, (size_t)numPoints * 8);
        uint16_t speed = rd_u16(&rd);
        uint16_t nameLen = rd_u16(&rd);
        const uint8_t *name = rd_block(&rd, nameLen);
        if (!rd.ok) {
            /* Tile truncado: se deja de leer y vale lo ya puntuado —perder un tramo
             * bueno por un archivo dañado al final sería peor—, pero ahora queda
             * contado en lugar de pasar inadvertido. */
            stats->tiles_truncated++;
            break;
        }

        for (uint16_t p = 0; p + 1 < numPoints; p++) {
            float d = distance_point_to_segment_haversine(
                        lat, lon,
                        PT_LAT(pts, p),   PT_LON(pts, p),
                        PT_LAT(pts, p+1), PT_LON(pts, p+1)
                    );

            if (d > MAX_STREET_DISTANCE)
                continue;

            /* Skip heading math when this edge cannot beat current best.
             * Strictly this should be `localBestScore + STICK_BONUS_M`, like the
             * check below, since the locked street still gets its bonus applied
             * later -- but replaying the 6 reference logs gives byte-identical
             * matches either way, so it stays as the cheaper form. */
            if (d > segBestScore && d > localBestScore)
                continue;

            float sc = d;
            if (use_heading) {
                if (d + max_hdg_penalty < segBestScore || d + max_hdg_penalty < localBestScore + STICK_BONUS_M) {
                    sc = d + HEADING_WEIGHT_M_PER_DEG * heading_err_deg(
                            cog_deg, PT_LAT(pts, p), PT_LON(pts, p),
                            PT_LAT(pts, p+1), PT_LON(pts, p+1));
                } else {
                    continue;
                }
            }

            if (sc < segBestScore) {
                segBestScore = sc;
                segBestDist = d;
            }
        }

        bool validStreet = (nameLen > 0 && name && name[0] != 0);

        if (validStreet)
            *outTileHasData = true;

        if (!validStreet && speed > 0 && segBestDist <= MAX_STREET_DISTANCE &&
            segBestDist < localAnonDist) {
            localAnonDist = segBestDist;
            localAnonSpeed = speed;
        }

        if (!validStreet || segBestDist > MAX_STREET_DISTANCE)
            continue;

        /* Stickiness keys on name *and* posted limit. OSM often names a
         * motorway and its service road identically -- around Acceso Norte two
         * parallel ways 10-15 m apart are both "Acceso Norte", one tagged 130
         * and the other 80. Keying on the name alone handed both the same bonus,
         * so between them there was no hysteresis at all and the displayed limit
         * flipped with raw geometry even when the two were 0.7 m apart. */
        float score = segBestScore;
        /* The name lives in the cached buffer and is not NUL terminated, so the
         * match has to be length-exact rather than a plain strncmp. */
        if (preferred_name && preferred_name[0] &&
            speed == preferred_speed &&
            strncmp(preferred_name, (const char *)name, nameLen) == 0 &&
            preferred_name[nameLen] == '\0') {
            score -= STICK_BONUS_M;
            if (score < 0.0f)
                score = 0.0f;
        }

        if (score < localBestScore) {
            localBestScore = score;
            localBestDist = segBestDist;
            localBestSpeed = speed;
            size_t n = nameLen < sizeof(localBestName) - 1 ? nameLen
                                                           : sizeof(localBestName) - 1;
            memcpy(localBestName, name, n);
            localBestName[n] = 0;
        }

    }

    *outBestScore = localBestScore;
    *outBestDist = localBestDist;
    *outBestSpeed = localBestSpeed;
    *outAnonDist = localAnonDist;
    *outAnonSpeed = localAnonSpeed;
    strncpy(outBestName, localBestName, bestNameSize);
    outBestName[bestNameSize-1] = 0;

    return true;
}

void map_match_reset(map_match_state_t *st)
{
    if (st) {
        memset(st, 0, sizeof(*st));
    }
}

const char *map_limit_origin_name(map_limit_origin_t o)
{
    switch (o) {
    case MAP_LIMIT_NAMED_WAY: return "via_con_nombre";
    case MAP_LIMIT_ANON_WAY:  return "via_anonima";
    case MAP_LIMIT_NONE:
    default:                  return "ninguno";
    }
}

bool map_match_query(map_match_state_t *st, const map_tile_source_t *src,
                    const map_query_t *q, map_result_t *out)
{
    if (!st || !src || !src->fetch || !q || !out) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->generation = src->generation;

    /* Alias locales con los nombres de la versión anterior, para que el cuerpo del
     * algoritmo quede idéntico y el diff sea revisable. El estado ya no es `static`
     * de función: vive en `st` y se puede resetear. */
    const float lat = q->lat;
    const float lon = q->lon;
    const float cog_deg = q->cog_deg;
    const float speed_kmh = q->speed_kmh;
    char *const locked_name = st->locked_name;
    int  locked_speed = st->locked_speed;
    bool have_lock = st->have_lock;
    bool locked_anon = st->locked_anon;
    int *const outSpeed = &out->speed_kmh;
    char *const outStreet = out->street;
    const int maxStreetLen = (int)sizeof(out->street);

    /* Reject bogus near-null-island fixes (lat/lon ~ 0) that some NMEA
     * statements emit between real fixes. The device is in Argentina (~-34,-58)
     * and is never legitimately near 0,0 — without this each junk fix triggers a
     * full 9-tile neighbor scan of failing fopens, which is a big part of the lag. */
    if(fabsf(lat) < 1.0f && fabsf(lon) < 1.0f) {
        /* Fix cerca de null-island: se rechaza sin tocar la tarjeta ni el estado. */
        out->matched = false;
        out->limit_origin = MAP_LIMIT_NONE;
        return false;
    }

    bool use_heading = (speed_kmh >= HEADING_MIN_SPEED_KMH) &&
                       isfinite(cog_deg) && cog_deg >= 0.0f;
    const char *preferred = (have_lock && locked_name[0]) ? locked_name : NULL;
    const int preferred_speed = have_lock ? locked_speed : -1;

    int32_t lat_e7 = deg_to_e7(lat);
    int32_t lon_e7 = deg_to_e7(lon);

    int32_t base_origin_lat = tile_origin_e7(lat_e7);
    int32_t base_origin_lon = tile_origin_e7(lon_e7);

    float globalBestScore = 1e12f;
    float globalBestDist = 1e12f;
    int   globalBestSpeed = 0;
    char  globalBestName[128] = "";
    bool  foundAnything = false;
    float globalAnonDist = 1e12f;
    int   globalAnonSpeed = 0;

    double tile_min_lat = base_origin_lat / 1e7;
    double tile_max_lat = (base_origin_lat + TILE_SIZE_E7) / 1e7;
    double tile_min_lon = base_origin_lon / 1e7;
    double tile_max_lon = (base_origin_lon + TILE_SIZE_E7) / 1e7;

    float d_west  = haversine(lat, lon, lat, tile_min_lon);
    float d_east  = haversine(lat, lon, lat, tile_max_lon);
    float d_south = haversine(lat, lon, tile_min_lat, lon);
    float d_north = haversine(lat, lon, tile_max_lat, lon);

    {
        float tileBestScore, tileBestDist;
        int   tileBestSpeed;
        char  tileBestName[128];
        bool  tileHasData = false;
        float tileAnonDist = 1e12f;
        int   tileAnonSpeed = 0;

        if (scan_tile_for_match(src, out, base_origin_lat, base_origin_lon, lat, lon,
                                cog_deg, use_heading, preferred, preferred_speed,
                                &tileBestScore, &tileBestDist, &tileBestSpeed,
                                tileBestName, sizeof(tileBestName),
                                &tileHasData, &tileAnonDist, &tileAnonSpeed))
        {
            if (tileAnonDist < globalAnonDist) {
                globalAnonDist = tileAnonDist;
                globalAnonSpeed = tileAnonSpeed;
            }
            if (tileHasData && tileBestDist <= MAX_STREET_DISTANCE &&
                tileBestScore < globalBestScore) {
                globalBestScore = tileBestScore;
                globalBestDist = tileBestDist;
                globalBestSpeed = tileBestSpeed;
                strncpy(globalBestName, tileBestName, sizeof(globalBestName) - 1);
                globalBestName[sizeof(globalBestName) - 1] = 0;
                foundAnything = true;
            }
        }
    }

    /* Only skip neighbors when we are clearly on a segment (tight geometry). */
    if (!(foundAnything && globalBestDist <= EARLY_EXIT_DIST_M))
    {
        /* dx steps latitude and dy steps longitude (see ntx/nty below), so each
         * neighbour must be bounded by the distance to the edge it actually
         * lies across: dx pairs with d_south/d_north, dy with d_west/d_east.
         * Pairing them the other way round bounds the wrong axis and discards
         * tiles that are in fact adjacent -- with the car 0.2 m from the east
         * edge the check looked 330 m north, threw away both tiles holding the
         * street being driven on, and fell back to a cross street 32 m away. */
        struct {
            int dx, dy;
            float min_possible_dist;
        } neighbors[] = {
            {-1,  0, d_south},
            {+1,  0, d_north},
            { 0, -1, d_west},
            { 0, +1, d_east},
            {-1, -1, fminf(d_south, d_west)},
            {+1, -1, fminf(d_north, d_west)},
            {-1, +1, fminf(d_south, d_east)},
            {+1, +1, fminf(d_north, d_east)},
        };

        for (int i = 0; i < 8; i++) {
            if (neighbors[i].min_possible_dist >= globalBestDist &&
                neighbors[i].min_possible_dist >= MAX_STREET_DISTANCE)
                continue;
            if (foundAnything &&
                neighbors[i].min_possible_dist >= globalBestScore + STICK_BONUS_M)
                continue;

            int32_t ntx = base_origin_lat + neighbors[i].dx * TILE_SIZE_E7;
            int32_t nty = base_origin_lon + neighbors[i].dy * TILE_SIZE_E7;

            float tileBestScore, tileBestDist;
            int   tileBestSpeed;
            char  tileBestName[128];
            bool  tileHasData = false;
            float tileAnonDist = 1e12f;
            int   tileAnonSpeed = 0;

            if (!scan_tile_for_match(src, out, ntx, nty, lat, lon,
                                     cog_deg, use_heading, preferred, preferred_speed,
                                     &tileBestScore, &tileBestDist, &tileBestSpeed,
                                     tileBestName, sizeof(tileBestName),
                                     &tileHasData, &tileAnonDist, &tileAnonSpeed))
                continue;

            if (tileAnonDist < globalAnonDist) {
                globalAnonDist = tileAnonDist;
                globalAnonSpeed = tileAnonSpeed;
            }

            if (tileHasData && tileBestDist <= MAX_STREET_DISTANCE &&
                tileBestScore < globalBestScore) {
                globalBestScore = tileBestScore;
                globalBestDist = tileBestDist;
                globalBestSpeed = tileBestSpeed;
                strncpy(globalBestName, tileBestName, sizeof(globalBestName) - 1);
                globalBestName[sizeof(globalBestName) - 1] = 0;
                foundAnything = true;
            }
        }
    }

    if (!foundAnything || globalBestDist > MAX_STREET_DISTANCE) {
        /* No named way in range, but an unnamed one may still be right under us
         * -- on the pana2 run the car spends 12 s on a nameless 60 km/h link
         * whose closest *named* neighbour is 77 m away, past the cutoff. The old
         * code returned false there and offline_maps.c deliberately holds the
         * previous value, so the screen kept showing 130 on a 60 road. Report
         * the limit and keep the last street name rather than blanking it. */
        if (globalAnonSpeed > 0) {
            *outSpeed = globalAnonSpeed;
            locked_anon = true;
            strncpy(outStreet, locked_name, maxStreetLen);
            outStreet[maxStreetLen - 1] = 0;
            st->locked_speed = locked_speed;
            st->have_lock = have_lock;
            st->locked_anon = locked_anon;
            out->matched = true;
            out->limit_origin = MAP_LIMIT_ANON_WAY;
            out->anon_dist_m = globalAnonDist;
            out->best_dist_m = globalBestDist;
            return true;
        }
        *outSpeed = 0;
        outStreet[0] = 0;
        st->locked_speed = locked_speed;
        st->have_lock = have_lock;
        st->locked_anon = locked_anon;
        out->matched = false;
        out->limit_origin = MAP_LIMIT_NONE;
        return false;
    }

    strncpy(locked_name, globalBestName, sizeof(locked_name) - 1);
    locked_name[sizeof(locked_name) - 1] = 0;
    locked_speed = globalBestSpeed;
    have_lock = (locked_name[0] != 0);

    /* The name always comes from the named way, so the display never blanks.
     * The limit comes from whichever way we are actually on: if an unnamed one
     * is closer by more than the switch margin, it wins. Replayed against the
     * SD's own tiles this takes fixes whose limit came from >25 m away from 155
     * to 85, and the share of fixes showing a limit that is not the nearest
     * way's from 12.6% to 8.1%, for 13 more limit changes in 46 min.
     *
     * The choice needs its own hysteresis, for the same reason the street name
     * does. A fixed threshold flapped: on General Paz an unnamed 60 way sits
     * 0.04-0.45 m away while the named 80 centreline drifts between 1.7 and
     * 7.3 m, so `anon + 4 < named` went true, false, true on consecutive fixes
     * and the display bounced 60/80/60 with the car sitting on the same asphalt.
     * Widening the band in the direction of whatever is already shown costs
     * nothing in accuracy (7.1% either way over the six logs) and drops limit
     * changes from 46 to 40. */
    const float anon_margin = locked_anon ? -STICK_BONUS_M : STICK_BONUS_M;
    bool use_anon = (globalAnonSpeed > 0 &&
                     globalAnonDist + anon_margin < globalBestDist);
    *outSpeed = use_anon ? globalAnonSpeed : globalBestSpeed;
    locked_anon = use_anon;
    strncpy(outStreet, globalBestName, maxStreetLen);
    outStreet[maxStreetLen - 1] = 0;

    st->locked_speed = locked_speed;
    st->have_lock = have_lock;
    st->locked_anon = locked_anon;
    out->matched = true;
    out->limit_origin = use_anon ? MAP_LIMIT_ANON_WAY : MAP_LIMIT_NAMED_WAY;
    out->best_dist_m = globalBestDist;
    out->anon_dist_m = globalAnonDist;
    return true;
}
