#include "vehicle_io.h"

// All fields updated from the nmea_parser task, read from calculation_task and
// app_main. ESP32 int32 reads/writes are atomic (aligned), same pattern as vars.c.

static int32_t s_rpm            = 0;
static int32_t s_accel_pct      = 0;
static int32_t s_engine_load    = 0;
static int32_t s_brake_active   = 0;
static int32_t s_fuel_rate_x10  = 0;
static int32_t s_idling_sec     = 0;
static int32_t s_ignition       = 0;
static int32_t s_moving         = 0;
static int32_t s_ext_voltage_mv = 0;
static float   s_heading        = 0.0f;

void    set_io_rpm(int32_t v)            { s_rpm = v; }
int32_t get_io_rpm(void)                 { return s_rpm; }

void    set_io_accel_pct(int32_t v)      { s_accel_pct = v; }
int32_t get_io_accel_pct(void)           { return s_accel_pct; }

void    set_io_engine_load_pct(int32_t v){ s_engine_load = v; }
int32_t get_io_engine_load_pct(void)     { return s_engine_load; }

void    set_io_brake_active(bool v)      { s_brake_active = v ? 1 : 0; }
bool    get_io_brake_active(void)        { return s_brake_active != 0; }

void    set_io_fuel_rate_x10(int32_t v)  { s_fuel_rate_x10 = v; }
int32_t get_io_fuel_rate_x10(void)       { return s_fuel_rate_x10; }

void    set_io_idling_sec(int32_t v)     { s_idling_sec = v; }
int32_t get_io_idling_sec(void)          { return s_idling_sec; }

void    set_io_ignition(bool v)          { s_ignition = v ? 1 : 0; }
bool    get_io_ignition(void)            { return s_ignition != 0; }

void    set_io_moving(bool v)            { s_moving = v ? 1 : 0; }
bool    get_io_moving(void)              { return s_moving != 0; }

void    set_io_ext_voltage_mv(int32_t v) { s_ext_voltage_mv = v; }
int32_t get_io_ext_voltage_mv(void)      { return s_ext_voltage_mv; }

void    set_io_ruptela_heading(float v)  { s_heading = v; }
float   get_io_ruptela_heading(void)     { return s_heading; }
