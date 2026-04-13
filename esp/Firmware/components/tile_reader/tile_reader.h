#pragma once
#include <stdbool.h>
#include "sd_manager.h"


#define TILE_SIZE   0.003
#define TILE_INV    (1.0 / TILE_SIZE)                     // multiplication is faster and stable to perform than division
#define TILE_PATH   MOUNT_POINT "/tiles"

#define MAX_POINTS         256                 // max points per polyline segment
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

bool get_speed_and_name_at(float lat, float lon, int *outSpeed, char *outStreet, int maxStreetLen);

#ifdef __cplusplus
}
#endif
