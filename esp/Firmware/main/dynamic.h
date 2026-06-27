#pragma once

#include "lvgl.h"
#include "vars.h"

#define LIMIT_INDICATOR_MIN_VAL     10.0f
#define LIMIT_INDICATOR_MAX_VAL     110.0f
#define LIMIT_INDICATOR_LIMIT_VAL   103.0f

void set_street_name(const char *street_name);
void calculate_threshold(void);

// --- Anticipation (Stage 1): the road/limit the vehicle is about to enter ---
// Heading-projected lookahead result, written from the GPS loop and read by the
// UI/warning logic. `valid` is false when there is no usable prediction (vehicle
// stopped, no cog, or nothing matched ahead); consumers keep the last good value.
typedef struct {
    bool  valid;
    int   speed_limit;     // limit at the projected point, km/h (0 = untagged)
    int   distance_m;      // how far ahead the probe point was, meters
    char  street[128];     // street name at the projected point
} lookahead_t;

// Replace the current lookahead prediction (thread-safe copy).
void set_lookahead(const lookahead_t *la);
// Invalidate the prediction (e.g. vehicle stopped / cog not trustworthy).
void clear_lookahead(void);
// Snapshot the current prediction into *out. Returns out->valid for convenience.
bool get_lookahead(lookahead_t *out);

// Stage 2: if the heading-projected lookahead shows the speed limit dropping
// ahead, flash a one-shot "Límite NN en MMm" message on the street label. Edge-
// triggered (announces once per approach, no per-tick spam). Call periodically
// from the calculation task, alongside street_message_tick().
void lookahead_warning_tick(void);

// Show a temporary message on the street_name label for duration_ms, then the
// last real street name is restored (call street_message_tick periodically).
void show_temp_message(const char *text, int duration_ms);
void street_message_tick(void);
