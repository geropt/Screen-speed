#pragma once
#include <stdbool.h>
#include "sd_manager.h"


#define TILE_SIZE   0.003
#define TILE_INV    (1.0 / TILE_SIZE)                     // multiplication is faster and stable to perform than division
#define TILE_PATH   MOUNT_POINT "/tiles"

#define MAX_POINTS         8192                // sanity cap on polyline length (corruption guard)
#define MAX_STREET_NAME    128

#define EARTH_RADIUS       6371000.0f

// Distance cutoff limits
// for constraining the street limit from our location, in meters
// #define GPS_SPEED_LIMIT         25 to 40
// #define RURAL_ROADS_LIMIT       80 to 120
#define URBAN_STREET_LIMIT      50.0f
// #define HIGHWAY_LIMIT           150.0
#define MAX_STREET_DISTANCE     URBAN_STREET_LIMIT

#ifdef __cplusplus
extern "C" {
#endif

// heading_deg: vehicle course over ground (0-360°). Pass -1.0f when stopped or
// heading is unreliable; heading-based segment scoring is skipped in that case.
bool get_speed_and_name_at(float lat, float lon, float heading_deg,
                           int *outSpeed, char *outStreet, int maxStreetLen);

#ifdef __cplusplus
}
#endif
