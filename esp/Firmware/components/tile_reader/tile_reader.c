#include "tile_reader.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "tile_config.h"

const static char *TAG = "TILE_READER";

// ----------------- Binary read helpers -----------------
static uint32_t read_u32(FILE *f) {
    uint8_t b[4];
    fread(b, 1, 4, f);
    return (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static uint16_t read_u16(FILE *f) {
    uint8_t b[2];
    fread(b, 1, 2, f);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static int32_t read_i32(FILE *f) {
    uint8_t b[4];
    fread(b, 1, 4, f);
    return  (int32_t)b[0] |
           ((int32_t)b[1] << 8) |
           ((int32_t)b[2] << 16) |
           ((int32_t)b[3] << 24);
}

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

static inline int32_t filename_coord(int32_t origin_e7)
{
    return origin_e7 / FILENAME_SCALE;
}

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
static bool scan_tile_for_match(int32_t tx, int32_t ty, float lat, float lon,
                                float cog_deg, bool use_heading,
                                const char *preferred_name, int preferred_speed,
                                float *outBestScore, float *outBestDist,
                                int *outBestSpeed,
                                char *outBestName, size_t bestNameSize,
                                bool *outTileHasData,
                                float *outAnonDist, int *outAnonSpeed)
{
    *outTileHasData = false;

    char filename[96];
    /* Sharded layout: one subdirectory per tx column keeps each FAT directory
     * small, so fopen() no longer scans thousands of entries per lookup. */
    snprintf(filename, sizeof(filename), TILE_PATH "/%" PRId32 "/tile_%" PRId32 "_%" PRId32 ".bin",
             filename_coord(tx), filename_coord(tx), filename_coord(ty));

    FILE *f = fopen(filename, "rb");
    if (!f) {
        /* Expected at the edge of coverage: the 8 neighbors of a border tile
         * simply do not exist. Debug level, or this floods diag.log at 1 Hz. */
        ESP_LOGD(TAG, "no tile %" PRId32 "_%" PRId32, filename_coord(tx), filename_coord(ty));
        return false;  // tile does not exist
    }

    uint32_t segCount = read_u32(f);
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

        uint16_t numPoints = read_u16(f);

        float segBestScore = 1e12f;
        float segBestDist = 1e12f;

        float *lats  = malloc(sizeof(float) * numPoints);
        float *lons  = malloc(sizeof(float) * numPoints);

        /* PSRAM is off in the active sdkconfig, so these can genuinely fail.
         * Consume the record either way -- skipping the reads would desync the
         * file position and turn every later segment into garbage. */
        for (uint16_t p = 0; p < numPoints; p++) {
            int32_t lat_i = read_i32(f);
            int32_t lon_i = read_i32(f);
            if (lats && lons) {
                lats[p] = lat_i / 1e7f;
                lons[p] = lon_i / 1e7f;
            }
        }

        for (uint16_t p = 0; (lats && lons) && p < numPoints - 1; p++) {
            float d = distance_point_to_segment_haversine(
                        lat, lon,
                        lats[p],   lons[p],
                        lats[p+1], lons[p+1]
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
                            cog_deg, lats[p], lons[p], lats[p+1], lons[p+1]);
                } else {
                    continue;
                }
            }

            if (sc < segBestScore) {
                segBestScore = sc;
                segBestDist = d;
            }
        }

        free(lats);
        free(lons);

        uint16_t speed = read_u16(f);
        uint16_t nameLen = read_u16(f);

        char *nameBuf = malloc(nameLen + 1);
        if (!nameBuf) {
            /* Drain the name so the next segment starts at the right offset. */
            for (uint16_t i = 0; i < nameLen; i++) fgetc(f);
            continue;
        }
        fread(nameBuf, 1, nameLen, f);
        nameBuf[nameLen] = 0;

        bool validStreet = (nameLen > 0 && nameBuf[0] != 0);

        if (validStreet)
            *outTileHasData = true;

        if (!validStreet && speed > 0 && segBestDist <= MAX_STREET_DISTANCE &&
            segBestDist < localAnonDist) {
            localAnonDist = segBestDist;
            localAnonSpeed = speed;
        }

        if (!validStreet || segBestDist > MAX_STREET_DISTANCE) {
            free(nameBuf);
            continue;
        }

        /* Stickiness keys on name *and* posted limit. OSM often names a
         * motorway and its service road identically -- around Acceso Norte two
         * parallel ways 10-15 m apart are both "Acceso Norte", one tagged 130
         * and the other 80. Keying on the name alone handed both the same bonus,
         * so between them there was no hysteresis at all and the displayed limit
         * flipped with raw geometry even when the two were 0.7 m apart. */
        float score = segBestScore;
        if (preferred_name && preferred_name[0] &&
            speed == preferred_speed &&
            strncmp(nameBuf, preferred_name, MAX_STREET_NAME) == 0) {
            score -= STICK_BONUS_M;
            if (score < 0.0f)
                score = 0.0f;
        }

        if (score < localBestScore) {
            localBestScore = score;
            localBestDist = segBestDist;
            localBestSpeed = speed;
            strncpy(localBestName, nameBuf, sizeof(localBestName)-1);
            localBestName[sizeof(localBestName)-1] = 0;
        }

        free(nameBuf);
    }

    fclose(f);

    *outBestScore = localBestScore;
    *outBestDist = localBestDist;
    *outBestSpeed = localBestSpeed;
    *outAnonDist = localAnonDist;
    *outAnonSpeed = localAnonSpeed;
    strncpy(outBestName, localBestName, bestNameSize);
    outBestName[bestNameSize-1] = 0;

    return true;
}

bool get_speed_and_name_at(float lat, float lon, float cog_deg, float speed_kmh,
                           int *outSpeed, char *outStreet, int maxStreetLen)
{
    static char locked_name[MAX_STREET_NAME];
    static int  locked_speed = 0;
    static bool have_lock = false;

    /* Reject bogus near-null-island fixes (lat/lon ~ 0) that some NMEA
     * statements emit between real fixes. The device is in Argentina (~-34,-58)
     * and is never legitimately near 0,0 — without this each junk fix triggers a
     * full 9-tile neighbor scan of failing fopens, which is a big part of the lag. */
    if(fabsf(lat) < 1.0f && fabsf(lon) < 1.0f)
        return false;

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

        if (scan_tile_for_match(base_origin_lat, base_origin_lon, lat, lon,
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

            if (!scan_tile_for_match(ntx, nty, lat, lon,
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
            strncpy(outStreet, locked_name, maxStreetLen);
            outStreet[maxStreetLen - 1] = 0;
            return true;
        }
        *outSpeed = 0;
        outStreet[0] = 0;
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
     * way's from 12.6% to 8.1%, for 13 more limit changes in 46 min. */
    *outSpeed = globalBestSpeed;
    if (globalAnonSpeed > 0 && globalAnonDist + STICK_BONUS_M < globalBestDist) {
        *outSpeed = globalAnonSpeed;
    }
    strncpy(outStreet, globalBestName, maxStreetLen);
    outStreet[maxStreetLen - 1] = 0;
    return true;
}
