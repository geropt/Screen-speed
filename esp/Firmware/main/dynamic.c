#include "dynamic.h"
#include "screens.h"
#include "eez-flow.h"
#include "waveshare_amoled_lcd_port.h"  // lvgl_lock / lvgl_unlock
#include <stdio.h>
#include "esp_log.h"
#include "time.h"

// Periodo de un ciclo completo (apagado->encendido->apagado) del titileo, en ms.
#define OVERSPEED_RING_BLINK_MS  300

static bool s_ring_blinking = false;

static void ring_opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_INDICATOR);
}

// Prende o apaga el titileo del anillo de exceso de velocidad. Idempotente:
// llamar con el mismo estado no reinicia la animacion.
static void update_overspeed_ring(bool overspeed)
{
    if (overspeed == s_ring_blinking)
        return;
    s_ring_blinking = overspeed;

    if (!lvgl_lock(-1))
        return;

    lv_anim_del(objects.overspeed_ring, ring_opa_anim_cb);

    if (overspeed)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.overspeed_ring);
        lv_anim_set_exec_cb(&a, ring_opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, OVERSPEED_RING_BLINK_MS);
        lv_anim_set_playback_time(&a, OVERSPEED_RING_BLINK_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    else
    {
        lv_obj_set_style_arc_opa(objects.overspeed_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }

    lvgl_unlock();
}

void set_street_name(const char *street_name)
{
    lv_label_set_text(objects.street_name, street_name);
}

void calculate_threshold(void)
{
    int limit = get_var_speed_limit_value();
    int cur_speed = get_var_current_speed_value();

    bool overspeed = (limit > 0) && (cur_speed > limit);
    update_overspeed_ring(overspeed);
}
