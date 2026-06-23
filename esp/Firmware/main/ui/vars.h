#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

// Native global variables

extern int32_t get_var_indicator_threshold_value();
extern void set_var_indicator_threshold_value(int32_t value);
extern int32_t get_var_current_speed_value();
extern void set_var_current_speed_value(int32_t value);
extern int32_t get_var_speed_limit_value();
extern void set_var_speed_limit_value(int32_t value);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/