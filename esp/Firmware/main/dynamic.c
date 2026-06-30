#include "dynamic.h"
#include "screens.h"
#include "eez-flow.h"
#include "waveshare_amoled_lcd_port.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "time.h"

// Last real street name received from the GPS loop. Kept up to date even while a
// temporary on-screen message is being shown, so it can be restored afterwards.
static char last_street[128] = "";
// Timestamp (us) until which a temporary message owns the street_name label.
// 0 means "no message active".
static volatile int64_t msg_until_us = 0;

// Full-circle (360°) red ring drawn over the main screen edge. It is hidden
// (transparent) while under the speed limit and pulses red while over it. Created
// in our own code instead of EEZ's screens.c so a UI regeneration won't clobber it.
static lv_obj_t *s_overspeed_ring = NULL;

void overspeed_ring_init(void)
{
    if (lvgl_lock(-1))
    {
        lv_obj_t *r = lv_arc_create(objects.main);
        lv_obj_set_pos(r, 13, 13);
        lv_obj_set_size(r, 440, 440);          // same radius as the EEZ speed arcs
        lv_arc_set_bg_angles(r, 0, 360);       // full circle
        lv_arc_set_rotation(r, 0);
        lv_obj_remove_style(r, NULL, LV_PART_KNOB);       // no draggable knob
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(r, lv_color_hex(0xc1140d), LV_PART_MAIN);
        lv_obj_set_style_arc_width(r, 20, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);       // starts hidden
        lv_obj_set_style_arc_opa(r, LV_OPA_TRANSP, LV_PART_INDICATOR);  // indicator unused
        s_overspeed_ring = r;

        // The legacy EEZ speed arcs are unused now (replaced by this ring). Even at
        // value 0 they leave a ~1px red sliver where they meet at the top (270°), so
        // hide them outright instead of relying on the empty value.
        lv_obj_add_flag(objects.obj0__arc_indicator_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.obj0__arc_indicator_right, LV_OBJ_FLAG_HIDDEN);

        lvgl_unlock();
    }
}

static void write_street_label(const char *text)
{
    // lv_* APIs are not thread-safe: this runs outside the LVGL task,
    // so it must be guarded by the LVGL mutex.
    if (lvgl_lock(-1))
    {
        lv_label_set_text(objects.street_name, text);
        lvgl_unlock();
    }
}

void set_street_name(const char *street_name)
{
    // Always remember the latest real street name...
    strncpy(last_street, street_name, sizeof(last_street) - 1);
    last_street[sizeof(last_street) - 1] = '\0';

    // ...but don't paint it while a temporary message is on screen.
    if (esp_timer_get_time() >= msg_until_us)
        write_street_label(street_name);
}

void show_temp_message(const char *text, int duration_ms)
{
    write_street_label(text);
    msg_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}

void street_message_tick(void)
{
    if (msg_until_us != 0 && esp_timer_get_time() >= msg_until_us)
    {
        msg_until_us = 0;
        write_street_label(last_street);
    }
}

void calculate_threshold(void)
{
    int limit = get_var_speed_limit_value();
    int cur_speed = get_var_current_speed_value();

    // The EEZ-generated speed arcs are no longer used as a gradual gauge: keep
    // them empty (value 0) so they draw nothing. The overspeed feedback is the
    // full-circle ring below instead.
    set_var_indicator_threshold_value(0);

    // Over-speed state with hysteresis to prevent flicker at the boundary:
    // turn ON above the limit, turn OFF only once a couple km/h below it. An
    // unknown limit (untagged road, limit <= 0) never triggers the warning.
    static bool warning_on = false;
    if (limit <= 0)
        warning_on = false;
    else if (!warning_on && cur_speed > limit)
        warning_on = true;
    else if (warning_on && cur_speed <= limit - 2)
        warning_on = false;

    // Pulsing opacity for the red ring while over the limit (sine, ~0.6 s period).
    lv_opa_t ring_opa = LV_OPA_TRANSP;
    if (warning_on)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        float t = (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f;
        float period = 0.6f;
        // sine normalized to [0,1], then mapped to [LV_OPA_40 .. LV_OPA_COVER]
        float s = 0.5f * (1.0f + sinf(2.0f * 3.14159265f * t / period));
        int opa = (int)(LV_OPA_40 + s * (LV_OPA_COVER - LV_OPA_40));
        ring_opa = (lv_opa_t)opa;
    }

    if (lvgl_lock(-1))
    {
        if (s_overspeed_ring)
            lv_obj_set_style_arc_opa(s_overspeed_ring, ring_opa, LV_PART_MAIN);

        if (warning_on)
            lv_obj_clear_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
        lvgl_unlock();
    }
}
