#include "splash.h"
#include "mykeego_logo.h"
#include "screens.h"                    // objects.main (destino de la transicion)
#include "waveshare_amoled_lcd_port.h"  // lvgl_lock / lvgl_unlock, LCD_H_RES
#include <lvgl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Tiempo minimo en pantalla para que el logo sea visible aunque el arranque
// sea muy rapido. splash_finish() espera lo que falte antes de transicionar.
#define SPLASH_MIN_MS  1500

// Color de marca mykeego (cian del logo).
#define MYKEEGO_CYAN  lv_color_hex(0x00D8D8)
#define SPLASH_BG     lv_color_hex(0x000000)
#define BAR_TRACK     lv_color_hex(0x1E1E1E)

static lv_obj_t *s_scr   = NULL;  // pantalla de splash
static lv_obj_t *s_arc   = NULL;  // anillo de progreso (todo el borde)
static bool s_finished   = false;
static int64_t s_show_us = 0;  // instante en que se mostro el splash

// Anima el valor del arco de forma suave entre cambios de progreso.
static void arc_anim_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}

void splash_show(void)
{
    if (!lvgl_lock(-1))
        return;

    if (s_scr) {  // ya creado
        lvgl_unlock();
        return;
    }

    s_finished = false;
    s_show_us = esp_timer_get_time();

    // Pantalla propia, fondo negro, sin scroll ni bordes.
    s_scr = lv_obj_create(NULL);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, SPLASH_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);

    // Anillo de progreso: ocupa todo el borde de la pantalla circular. Va primero
    // para que el logo y el texto queden por encima en el centro.
    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, LCD_H_RES - 8, LCD_V_RES - 8);  // ~458x458, margen al borde
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);            // empieza arriba (12 en punto)
    lv_arc_set_bg_angles(s_arc, 0, 360);        // track = circulo completo
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);          // sin perilla
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);         // no interactivo
    // Track (circulo de fondo)
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, BAR_TRACK, LV_PART_MAIN);
    // Indicador (progreso cian)
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, MYKEEGO_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    // Logo (mascara alfa recoloreada a cian), centrado y algo por encima del medio.
    lv_obj_t *logo = lv_img_create(s_scr);
    lv_img_set_src(logo, &mykeego_logo);
    lv_obj_set_style_img_recolor(logo, MYKEEGO_CYAN, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(logo);

    lv_scr_load(s_scr);

    lvgl_unlock();
}

void splash_set_progress(uint8_t pct)
{
    if (pct > 100)
        pct = 100;

    if (!lvgl_lock(-1))
        return;

    if (s_scr && !s_finished && s_arc) {
        // Anima el arco desde el valor actual hasta el nuevo (suave).
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_arc);
        lv_anim_set_exec_cb(&a, arc_anim_cb);
        lv_anim_set_values(&a, lv_arc_get_value(s_arc), pct);
        lv_anim_set_time(&a, 400);
        lv_anim_start(&a);
    }

    lvgl_unlock();
}

void splash_finish(void)
{
    // Garantiza un tiempo minimo de logo en pantalla (fuera del lock para no
    // bloquear la tarea LVGL durante la espera).
    if (s_show_us && !s_finished) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_show_us) / 1000;
        if (elapsed_ms < SPLASH_MIN_MS)
            vTaskDelay(pdMS_TO_TICKS(SPLASH_MIN_MS - elapsed_ms));
    }

    if (!lvgl_lock(-1))
        return;

    if (!s_scr || s_finished) {
        lvgl_unlock();
        return;
    }
    s_finished = true;

    if (s_arc) {
        lv_anim_del(s_arc, arc_anim_cb);  // cancela cualquier anim en curso
        lv_arc_set_value(s_arc, 100);     // anillo completo
    }

    // Fundido hacia la pantalla principal. auto_del=true borra el splash (pantalla
    // saliente) al terminar la animacion, liberando su memoria.
    lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_FADE_ON, 400, 150, true);

    s_scr   = NULL;
    s_arc   = NULL;

    lvgl_unlock();
}
