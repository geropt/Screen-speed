#pragma once
#include <stdbool.h>
#include <stdint.h>

void     set_io_rpm(int32_t v);
int32_t  get_io_rpm(void);

void     set_io_accel_pct(int32_t v);
int32_t  get_io_accel_pct(void);

void     set_io_engine_load_pct(int32_t v);
int32_t  get_io_engine_load_pct(void);

void     set_io_brake_active(bool v);
bool     get_io_brake_active(void);

void     set_io_fuel_rate_x10(int32_t v);  // L/h × 10
int32_t  get_io_fuel_rate_x10(void);

void     set_io_idling_sec(int32_t v);
int32_t  get_io_idling_sec(void);

void     set_io_ignition(bool v);
bool     get_io_ignition(void);

void     set_io_moving(bool v);
bool     get_io_moving(void);

void     set_io_ext_voltage_mv(int32_t v);
int32_t  get_io_ext_voltage_mv(void);

void     set_io_ruptela_heading(float v);  // degrees 0–360
float    get_io_ruptela_heading(void);
