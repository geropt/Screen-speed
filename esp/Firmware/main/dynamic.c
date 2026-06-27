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

// --- Anticipation (Stage 1) shared state ---------------------------------
// Written by the GPS loop, read by the UI/warning logic. Guarded by the LVGL
// mutex (reused here as a cheap, already-available lock) so the multi-field
// struct is never read half-updated.
static lookahead_t s_lookahead = { .valid = false };

void set_lookahead(const lookahead_t *la)
{
    if (lvgl_lock(-1))
    {
        s_lookahead = *la;
        s_lookahead.valid = true;
        lvgl_unlock();
    }
}

void clear_lookahead(void)
{
    if (lvgl_lock(-1))
    {
        s_lookahead.valid = false;
        lvgl_unlock();
    }
}

bool get_lookahead(lookahead_t *out)
{
    bool valid = false;
    if (lvgl_lock(-1))
    {
        *out = s_lookahead;
        valid = s_lookahead.valid;
        lvgl_unlock();
    }
    return valid;
}

// --- Anticipation (Stage 2): lower-limit-ahead pre-warning ---------------
// How much the upcoming limit must drop (km/h) before we bother warning, so a
// 50->48 rounding artifact never fires.
#define LOOKAHEAD_LIMIT_DROP_MIN   10
// Minimum on-screen time for the pre-warning, ms.
#define LOOKAHEAD_WARN_MS          3000

void lookahead_warning_tick(void)
{
    // Edge-triggered: remember the limit we last warned about so we announce a
    // given drop only once per approach, not on every 100 ms tick.
    static int warned_limit = 0;

    lookahead_t la;
    if (!get_lookahead(&la) || la.speed_limit <= 0)
    {
        // No usable prediction (stopped / nothing matched ahead): rearm.
        warned_limit = 0;
        return;
    }

    int cur_limit = get_var_speed_limit_value();

    // Fire only when the road ahead is meaningfully slower than the current one.
    bool dropping = (cur_limit > 0) &&
                    (la.speed_limit < cur_limit - LOOKAHEAD_LIMIT_DROP_MIN);

    if (dropping)
    {
        // New (or further-reduced) limit ahead -> announce once.
        if (la.speed_limit != warned_limit)
        {
            warned_limit = la.speed_limit;
            char msg[48];
            snprintf(msg, sizeof(msg), "Limite %d en %dm",
                     la.speed_limit, la.distance_m);
            show_temp_message(msg, LOOKAHEAD_WARN_MS);
        }
    }
    else
    {
        // Once the current limit has caught up to (or matches) the prediction,
        // the approach is over: rearm for the next drop.
        warned_limit = 0;
    }
}

void calculate_threshold(void)
{
    int limit = get_var_speed_limit_value();
    int cur_speed = get_var_current_speed_value();

    // Guard against division by zero (untagged roads export speed_limit == 0).
    if (limit <= 0)
    {
        set_var_indicator_threshold_value((int)LIMIT_INDICATOR_MIN_VAL);
        if (lvgl_lock(-1))
        {
            lv_obj_add_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
            lvgl_unlock();
        }
        return;
    }

    float ratio = (float)cur_speed / (float)limit;
    float thresh;

    if ((ratio < 1.0f))
    {
        // Exponential growth
        float k = 3.0f;         // tuning factor for curve steepness
        thresh = LIMIT_INDICATOR_MIN_VAL + LIMIT_INDICATOR_LIMIT_VAL * powf(ratio, k);
    }
    else
    {
        // Time-based oscillation for ratio >= 1
        // Get a time variable in seconds
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        float t = (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f; // current time in seconds

        // Oscillate between min and max
        float osc_min = LIMIT_INDICATOR_LIMIT_VAL;
        float osc_max = LIMIT_INDICATOR_MAX_VAL;
        float period = 0.5f; // period in seconds for one full oscillation

        // sine oscillation normalized to [0,1]
        float sine_val = 0.4f * (1.0f + sinf(2.0f * 3.14159265f * t / period));
        thresh = osc_min + sine_val * (osc_max - osc_min);

        if (thresh > LIMIT_INDICATOR_MAX_VAL)
        {
            thresh = LIMIT_INDICATOR_MAX_VAL;
        }
    }

    set_var_indicator_threshold_value((int)thresh);

    // Over-speed visual warning. Hysteresis prevents flicker at the boundary:
    // turn ON above limit, turn OFF only once a couple km/h below it.
    static bool warning_on = false;
    if (!warning_on && cur_speed > limit)
        warning_on = true;
    else if (warning_on && cur_speed <= limit - 2)
        warning_on = false;

    if (lvgl_lock(-1))
    {
        if (warning_on)
            lv_obj_clear_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
        lvgl_unlock();
    }
}
