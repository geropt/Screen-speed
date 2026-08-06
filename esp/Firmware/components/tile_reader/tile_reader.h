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

/* Map-match tuning: heading + stickiness (meters / degrees / km/h). */
#define HEADING_WEIGHT_M_PER_DEG  0.20f  /* 45° ≈ +9 m score */
/* Switch margin: how much better a challenger must score to take over the
 * display. Dual carriageways can run 6-8 m apart (Acceso Norte / its colectora),
 * so 12 m was wider than the real separation and held the colectora on screen
 * for 26 s after merging onto the 130 lanes. Measured over 6 real logs the good
 * plateau is 3-6 m; 5 sits in the middle of it. */
#define STICK_BONUS_M             5.0f   /* prefer last locked street */
#define HEADING_MIN_SPEED_KMH     8.0f   /* COG unreliable when slower */
#define EARLY_EXIT_DIST_M         15.0f  /* only skip neighbors if this close */

#ifdef __cplusplus
extern "C" {
#endif

bool get_speed_and_name_at(float lat, float lon, float cog_deg, float speed_kmh,
                           int *outSpeed, char *outStreet, int maxStreetLen);

#ifdef __cplusplus
}
#endif
