#include "dynamic.h"
#include "screens.h"
#include "eez-flow.h"
#include <stdio.h>
#include "esp_log.h"
#include "time.h"


void set_street_name(const char *street_name)
{
    lv_label_set_text(objects.street_name, street_name);
}

void calculate_threshold(void)
{
    int limit = get_var_speed_limit_value();
    int cur_speed = get_var_current_speed_value();

    float ratio = (float)cur_speed / (float)limit;
    float thresh;
    bool flag = false;

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
            flag = true;
            thresh = LIMIT_INDICATOR_MAX_VAL;
        }
    }

    set_var_indicator_threshold_value((int)thresh);

    // // also set the warning label
    // lv_obj_clear_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);
    // lv_obj_add_flag(objects.speed_limit_warning_label, LV_OBJ_FLAG_HIDDEN);

    // ESP_LOGI("calculation_task",
    //      "limit: %" PRId32 " \t cur_speed: %" PRId32 " \t ratio: %.3f \t thresh: %" PRId32 "\t flag: %s",
    //      (int32_t)limit,
    //      (int32_t)cur_speed,
    //      (double)ratio,
    //      (int32_t)thresh,
    //      (flag) ? "true" : "false");
}
