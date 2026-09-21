#pragma once

#include "lvgl.h"
#include "vars.h"

void set_street_name(const char *street_name);
void calculate_threshold(void);

// Show a temporary message on the street_name label for duration_ms, then the
// last real street name is restored (call street_message_tick periodically).
void show_temp_message(const char *text, int duration_ms);
void street_message_tick(void);
