#include "tile_reader.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "tile_config.h"

const static char *TAG = "TILE_READER";

// ----------------- Binary read helpers -----------------
// Each returns false on a short read (truncated/corrupt tile) so callers can
// bail out instead of trusting garbage.
static bool read_u32(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return true;
}

static bool read_u16(FILE *f, uint16_t *out) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return false;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

static bool read_i32(FILE *f, int32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (int32_t)b[0] |
           ((int32_t)b[1] << 8) |
           ((int32_t)b[2] << 16) |
           ((int32_t)b[3] << 24);
    return true;
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

// // ------------------ distance (fast) ------------------
// static float dist2(float lat1, float lon1, float lat2, float lon2) {
//     float dx = lon1 - lon2;
//     float dy = lat1 - lat2;
//     return dx*dx + dy*dy;
// }

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

// --------------------------------------------------------
//         MAIN FUNCTION (called by the rest of app)
// --------------------------------------------------------
bool scan_tile_for_match(int32_t tx, int32_t ty, float lat, float lon,
                         float *outBestDist, int *outBestSpeed,
                         char *outBestName, size_t bestNameSize,
                         bool *outTileHasData)
{
    *outTileHasData = false;

    char filename[64];
    snprintf(filename, sizeof(filename), TILE_PATH "/tile_%ld_%ld.bin", filename_coord(tx), filename_coord(ty));
    // ESP_LOGI(TAG, "Opening file /tile_%d_%d.bin", tx, ty);

    FILE *f = fopen(filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Error reading file /tile_%ld_%ld.bin", filename_coord(tx), filename_coord(ty));
        return false;  // tile does not exist
    }

    char nameBuf[MAX_STREET_NAME + 1];

    uint32_t segCount;
    if (!read_u32(f, &segCount)) {
        ESP_LOGE(TAG, "Corrupt tile (header) %s", filename);
        fclose(f);
        return false;
    }

    float localBestDist = 1e12f;
    int   localBestSpeed = 0;
    char  localBestName[128] = "";

    for (uint32_t s = 0; s < segCount; s++) {

        uint16_t numPoints;
        if (!read_u16(f, &numPoints)) {
            ESP_LOGE(TAG, "Corrupt tile (numPoints) %s", filename);
            fclose(f);
            return false;
        }

        // Sanity bound: a polyline far larger than any real OSM way means the
        // stream is misaligned/corrupt. (MAX_POINTS is a generous upper limit.)
        if (numPoints < 2 || numPoints > MAX_POINTS) {
            ESP_LOGE(TAG, "Corrupt tile (numPoints=%u) %s", numPoints, filename);
            fclose(f);
            return false;
        }

        float segBestDist = 1e12f;

        // Stream points one at a time, keeping only the previous one. This needs
        // no dynamic allocation and handles arbitrarily long polylines.
        float prevLat = 0, prevLon = 0;
        for (uint16_t p = 0; p < numPoints; p++) {
            int32_t lat_i, lon_i;
            if (!read_i32(f, &lat_i) || !read_i32(f, &lon_i)) {
                ESP_LOGE(TAG, "Corrupt tile (points) %s", filename);
                fclose(f);
                return false;
            }
            float curLat = lat_i / 1e7f;
            float curLon = lon_i / 1e7f;

            if (p > 0) {
                float d = distance_point_to_segment_haversine(
                            lat, lon, prevLat, prevLon, curLat, curLon);
                if (d < segBestDist)
                    segBestDist = d;
            }
            prevLat = curLat;
            prevLon = curLon;
        }

        uint16_t speed, nameLen;
        if (!read_u16(f, &speed) || !read_u16(f, &nameLen)) {
            ESP_LOGE(TAG, "Corrupt tile (meta) %s", filename);
            fclose(f);
            return false;
        }

        // Read up to MAX_STREET_NAME bytes; if the stored name is longer, keep
        // the prefix and skip the rest (don't treat a long name as corruption).
        uint16_t readLen = (nameLen > MAX_STREET_NAME) ? MAX_STREET_NAME : nameLen;
        if (fread(nameBuf, 1, readLen, f) != readLen) {
            ESP_LOGE(TAG, "Corrupt tile (name) %s", filename);
            fclose(f);
            return false;
        }
        nameBuf[readLen] = 0;

        if (nameLen > readLen && fseek(f, nameLen - readLen, SEEK_CUR) != 0) {
            ESP_LOGE(TAG, "Corrupt tile (name skip) %s", filename);
            fclose(f);
            return false;
        }

        // VALID STREET CHECK: skip unnamed segments (service roads, etc.)
        bool validStreet = (nameLen > 0 && nameBuf[0] != 0);
        if (!validStreet)
            continue;

        *outTileHasData = true;

        if (segBestDist < localBestDist) {
            localBestDist = segBestDist;
            localBestSpeed = speed;
            strncpy(localBestName, nameBuf, sizeof(localBestName)-1);
            localBestName[sizeof(localBestName)-1] = 0;
        }
    }

    fclose(f);

    // output best segment from THIS tile only
    *outBestDist = localBestDist;
    *outBestSpeed = localBestSpeed;
    strncpy(outBestName, localBestName, bestNameSize);
    outBestName[bestNameSize-1] = 0;

    return true;
}

bool get_speed_and_name_at(float lat, float lon, int *outSpeed,
                           char *outStreet, int maxStreetLen)
{
    if((lat == 0) && (lon == 0))
        return false;

    int32_t lat_e7 = deg_to_e7(lat);
    int32_t lon_e7 = deg_to_e7(lon);

    int32_t base_origin_lat = tile_origin_e7(lat_e7);
    int32_t base_origin_lon = tile_origin_e7(lon_e7);

    float globalBestDist = MAX_STREET_DISTANCE;
    int   globalBestSpeed = 0;
    char  globalBestName[128] = "";
    bool  foundAnything = false;

    // ---------------------------------------------------------
    // Compute tile bounds
    // ---------------------------------------------------------
    double tile_min_lat = base_origin_lat / 1e7;
    double tile_max_lat = (base_origin_lat + TILE_SIZE_E7) / 1e7;
    double tile_min_lon = base_origin_lon / 1e7;
    double tile_max_lon = (base_origin_lon + TILE_SIZE_E7) / 1e7;

    // Distance from point to tile edges
    float d_west  = haversine(lat, lon, lat, tile_min_lon);
    float d_east  = haversine(lat, lon, lat, tile_max_lon);
    float d_south = haversine(lat, lon, tile_min_lat, lon);
    float d_north = haversine(lat, lon, tile_max_lat, lon);

    // ---------------------------------------------------------
    // 1. Try central tile FIRST
    // ---------------------------------------------------------
    {
        float tileBestDist;
        int   tileBestSpeed;
        char  tileBestName[128];
        bool  tileHasData = false;

        if (scan_tile_for_match(base_origin_lat, base_origin_lon, lat, lon,
                                &tileBestDist, &tileBestSpeed, tileBestName, sizeof(tileBestName),
                                &tileHasData))
        {
            if (tileHasData && tileBestDist < globalBestDist) {
                printf("Data extracted from tile_%ld_%ld.bin\n", filename_coord(base_origin_lat), filename_coord(base_origin_lon));
                globalBestDist = tileBestDist;
                globalBestSpeed = tileBestSpeed;
                strncpy(globalBestName, tileBestName, sizeof(globalBestName));
                foundAnything = true;
            }
        }
    }

    // Early exit if already good enough
    if (foundAnything && globalBestDist <= MAX_STREET_DISTANCE)
        goto DONE;

    // ---------------------------------------------------------
    // 2. Scan neighbor tiles with geometric pruning
    // ---------------------------------------------------------
    struct {
        int dx, dy;
        float min_possible_dist;
    } neighbors[] = {
        {-1,  0, d_west},
        {+1,  0, d_east},
        { 0, -1, d_south},
        { 0, +1, d_north},
        {-1, -1, fminf(d_west,  d_south)},
        {+1, -1, fminf(d_east,  d_south)},
        {-1, +1, fminf(d_west,  d_north)},
        {+1, +1, fminf(d_east,  d_north)},
    };

    for (int i = 0; i < 8; i++) {
        // Tile cannot possibly beat current best
        if (neighbors[i].min_possible_dist >= globalBestDist)
            continue;

        int32_t ntx = base_origin_lat + neighbors[i].dx * TILE_SIZE_E7;
        int32_t nty = base_origin_lon + neighbors[i].dy * TILE_SIZE_E7;

        float tileBestDist;
        int   tileBestSpeed;
        char  tileBestName[128];
        bool  tileHasData = false;

        if (!scan_tile_for_match(ntx, nty, lat, lon,
                                 &tileBestDist, &tileBestSpeed,
                                 tileBestName, sizeof(tileBestName),
                                 &tileHasData))
            continue;

        if (tileHasData && tileBestDist < globalBestDist) {
            printf("Data extracted from tile_%ld_%ld.bin\n", filename_coord(ntx), filename_coord(nty));
            globalBestDist = tileBestDist;
            globalBestSpeed = tileBestSpeed;
            strncpy(globalBestName, tileBestName, sizeof(globalBestName));
            foundAnything = true;
        }
    }

DONE:
    // ---------------------------------------------------------
    // 3. Final decision
    // ---------------------------------------------------------
    if (!foundAnything || globalBestDist > MAX_STREET_DISTANCE) {
        // No tile contains valid streets
        printf("No street data found in this area.\n");
        *outSpeed = 0;
        outStreet[0] = 0;
        return false;
    }

    *outSpeed = globalBestSpeed;
    strncpy(outStreet, globalBestName, maxStreetLen);
    outStreet[maxStreetLen - 1] = 0;
    return true;
}

// --------------------------------------------------------
//         LOOKAHEAD (heading-projected anticipation)
// --------------------------------------------------------
void project_point_ahead(float lat, float lon, float cog_deg, float dist_m,
                         float *outLat, float *outLon)
{
    // Flat-earth offset over the short distances we look ahead (<=250 m): exact
    // enough and far cheaper than a great-circle destination formula. NMEA cog
    // is degrees clockwise from North, so North maps to +lat, East to +lon.
    float cog_rad = cog_deg * (float)M_PI / 180.0f;
    float dlat = (dist_m * cosf(cog_rad)) / EARTH_RADIUS;            // radians
    float coslat = cosf(lat * (float)M_PI / 180.0f);
    if (coslat < 1e-6f) coslat = 1e-6f;                             // guard near the poles
    float dlon = (dist_m * sinf(cog_rad)) / (EARTH_RADIUS * coslat); // radians

    *outLat = lat + dlat * 180.0f / (float)M_PI;
    *outLon = lon + dlon * 180.0f / (float)M_PI;
}

bool get_speed_and_name_at_lookahead(float lat, float lon, float cog_deg, float horizon_m,
                                     int *outSpeed, char *outStreet, int maxStreetLen,
                                     float *outProjLat, float *outProjLon)
{
    float plat, plon;
    project_point_ahead(lat, lon, cog_deg, horizon_m, &plat, &plon);

    if (outProjLat) *outProjLat = plat;
    if (outProjLon) *outProjLon = plon;

    // Reuse the existing matcher (also scans neighbour tiles at boundaries, so a
    // probe point that crosses a tile edge still resolves). Heap-safe: the
    // scanner streams polyline points one at a time, no allocation.
    return get_speed_and_name_at(plat, plon, outSpeed, outStreet, maxStreetLen);
}
