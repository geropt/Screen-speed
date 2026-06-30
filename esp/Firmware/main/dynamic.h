#pragma once

#include "lvgl.h"
#include "vars.h"

#define LIMIT_INDICATOR_MIN_VAL     10.0f
#define LIMIT_INDICATOR_MAX_VAL     110.0f
#define LIMIT_INDICATOR_LIMIT_VAL   103.0f

void set_street_name(const char *street_name);
void calculate_threshold(void);

// Create the full-circle red overspeed ring on objects.main. Must be called once
// after ui_init() (i.e. after waveshare_led_init()) and before calculate_threshold.
void overspeed_ring_init(void);

// Show a temporary message on the street_name label for duration_ms, then the
// last real street name is restored (call street_message_tick periodically).
void show_temp_message(const char *text, int duration_ms);
void street_message_tick(void);
