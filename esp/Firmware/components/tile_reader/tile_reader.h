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

bool get_speed_and_name_at(float lat, float lon, int *outSpeed, char *outStreet, int maxStreetLen);

// Project a point `dist_m` meters ahead of (lat,lon) along course `cog_deg`
// (NMEA course over ground: 0 = North, 90 = East, increasing clockwise).
// Outputs the projected coordinates in degrees. Pure helper (no I/O).
void project_point_ahead(float lat, float lon, float cog_deg, float dist_m,
                         float *outLat, float *outLon);

// Heading-projected lookahead: project a probe point `horizon_m` ahead along
// `cog_deg` and return the street + speed limit there, reusing
// get_speed_and_name_at() (so it still scans neighbour tiles at boundaries).
// outProjLat/outProjLon (may be NULL) receive the probe point for diagnostics.
// Returns true if a named street was matched at the projected point.
bool get_speed_and_name_at_lookahead(float lat, float lon, float cog_deg, float horizon_m,
                                     int *outSpeed, char *outStreet, int maxStreetLen,
                                     float *outProjLat, float *outProjLon);

#ifdef __cplusplus
}
#endif
