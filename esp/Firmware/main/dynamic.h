#pragma once

#include "lvgl.h"
#include "vars.h"

#define LIMIT_INDICATOR_MIN_VAL     10.0f
#define LIMIT_INDICATOR_MAX_VAL     110.0f
#define LIMIT_INDICATOR_LIMIT_VAL   103.0f

void set_street_name(const char *street_name);
void calculate_threshold(void);
