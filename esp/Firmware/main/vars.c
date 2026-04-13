#include "ui/vars.h"
#include "dynamic.h"

int32_t indicator_threshold = LIMIT_INDICATOR_MIN_VAL;
int32_t current_speed = 0;
int32_t speed_limit = 78;


int32_t get_var_indicator_threshold_value()
{
    return indicator_threshold;
}

void set_var_indicator_threshold_value(int32_t value)
{
    indicator_threshold = value;
}

int32_t get_var_current_speed_value()
{
    return current_speed;
}

void set_var_current_speed_value(int32_t value)
{
    current_speed = value;
}

int32_t get_var_speed_limit_value()
{
    return speed_limit;
}

void set_var_speed_limit_value(int32_t value)
{
    speed_limit = value;
}
