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

    uint32_t segCount = read_u32(f);
    float localBestDist = 1e12f;
    int   localBestSpeed = 0;
    char  localBestName[128] = "";

    for (uint32_t s = 0; s < segCount; s++) {

        uint16_t numPoints = read_u16(f);

        float segBestDist = 1e12f;

        // read all points into temporary local array
        float *lats  = malloc(sizeof(float) * numPoints);
        float *lons  = malloc(sizeof(float) * numPoints);

        for (uint16_t p = 0; p < numPoints; p++) {
            int32_t lat_i = read_i32(f);
            int32_t lon_i = read_i32(f);
            lats[p] = lat_i / 1e7f;
            lons[p] = lon_i / 1e7f;
        }

        // Compute minimum distance to ANY segment Pi → Pi+1
        for (uint16_t p = 0; p < numPoints - 1; p++) {

            float d = distance_point_to_segment_haversine(
                        lat, lon,
                        lats[p],   lons[p],
                        lats[p+1], lons[p+1]
                    );
            // ESP_LOGW(TAG, "new distance: %f", d);

            if (d < segBestDist)
            {
                segBestDist = d;
                // ESP_LOGI(TAG, "best distance updated; %f", d);
            }
        }

        free(lats);
        free(lons);

        uint16_t speed = read_u16(f);
        uint16_t nameLen = read_u16(f);

        char *nameBuf = malloc(nameLen + 1);
        if (nameBuf) {
            fread(nameBuf, 1, nameLen, f);
            nameBuf[nameLen] = 0;
        } else {
            for (uint16_t i = 0; i < nameLen; i++) fgetc(f);
            nameBuf = "";
        }

        // VALID STREET CHECK
        bool validStreet = (nameLen > 0 && nameBuf[0] != 0);

        if (validStreet)
            *outTileHasData = true;  // good

        // but ALSO check this:
        if (!validStreet)
            continue;

        if (segBestDist < localBestDist) {
            localBestDist = segBestDist;
            localBestSpeed = speed;
            strncpy(localBestName, nameBuf, sizeof(localBestName)-1);
            // ESP_LOGW(TAG, "updating fields name: %s \t speed: %d \t dist: %f", localBestName, localBestSpeed, localBestDist);
        }

        if (nameBuf && nameBuf != (char*)"")
            free(nameBuf);
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
