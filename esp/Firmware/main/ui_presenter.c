#include "ui_presenter.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "screens.h"
#include "ui/vars.h"
#include "res_metrics.h"

static const char *TAG = "UI_PRES";

/* Reparto de escritura de widgets, tal como está el proyecto EEZ.
 *
 * `tick_screen_main()` —código generado— reescribe `current_speed_label` y
 * `speed_limit_label` en cada tick a partir de las variables de flow. Pelear con
 * eso significaría dos escritores sobre el mismo widget, que es justo lo que P03
 * viene a eliminar. Así que el reparto es:
 *
 *   EEZ (generado)     current_speed_label, speed_limit_label
 *                      -> el presenter les habla a través de set_var_*()
 *
 *   presenter (acá)    speed_limit_warning_label  cualificador del límite
 *                      street_name                calle o mensaje de estado
 *                      speed_limit_container      visible sólo con límite usable
 *                      overspeed_ring             titileo de exceso
 *
 * Ninguno de los cuatro widgets del presenter es tocado por `tick_screen_main()`,
 * así que no hay solapamiento. Y no se modifica un archivo generado, con lo cual
 * regenerar desde EEZ no borra nada de esto.
 *
 * Un número no puede expresar «no sé»: poner 0 en el límite se leería como
 * «límite 0». Por eso, cuando el límite no es usable, el contenedor se esconde y
 * el cualificador dice qué pasa.
 */

#define OVERSPEED_RING_BLINK_MS 300

static SemaphoreHandle_t s_box_mux;
static ui_model_t s_pending;
static bool       s_has_pending;
static ui_model_t s_applied;
static bool       s_has_applied;
static bool       s_ring_blinking;

/* Traza recepción → snapshot → flush. `s_fix_mono_ms` lo fija la tarea principal
 * al recibir el fix; el callback de DMA del panel la cierra. `s_trace_armed` evita
 * atribuirle a un fix el flush de una animación que no tiene nada que ver. */
static volatile uint64_t s_fix_mono_ms;
static volatile bool     s_trace_armed;

esp_err_t ui_presenter_init(void)
{
    if (s_box_mux) {
        return ESP_OK;
    }
    s_box_mux = xSemaphoreCreateMutex();
    return s_box_mux ? ESP_OK : ESP_ERR_NO_MEM;
}

void ui_presenter_publish(const ui_model_t *model)
{
    if (!model || !s_box_mux) {
        return;
    }
    /* Espera acotada: si el presenter está copiando justo ahora, este modelo se
     * descarta y el siguiente lo reemplaza. La tarea que publica nunca espera a
     * LVGL ni al panel. */
    if (xSemaphoreTake(s_box_mux, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    s_pending = *model;
    s_has_pending = true;
    xSemaphoreGive(s_box_mux);
}

void ui_presenter_note_fix_received(uint64_t ms)
{
    s_fix_mono_ms = ms;
    s_trace_armed = true;
}

void ui_presenter_note_flush_done(void)
{
    if (!s_trace_armed) {
        return;
    }
    s_trace_armed = false;
    uint64_t started = s_fix_mono_ms;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (now >= started) {
        /* Meta del plan: p95 <200 ms desde el fix hasta el flush del dato. Se
         * guarda en µs como el resto de los canales. */
        res_metrics_sample_us(RES_CH_FIX_TO_DISPLAY, (uint32_t)((now - started) * 1000));
    }
}

/* ---------- traducción del modelo a widgets ---------- */

static bool model_differs(const ui_model_t *a, const ui_model_t *b)
{
    return a->screen != b->screen ||
           a->speed_state != b->speed_state ||
           a->speed_kmh != b->speed_kmh ||
           a->limit_state != b->limit_state ||
           a->limit_kmh != b->limit_kmh ||
           a->show_street != b->show_street ||
           a->overspeed != b->overspeed ||
           a->sd_present != b->sd_present ||
           strncmp(a->street, b->street, UI_STREET_MAX) != 0;
}

/* Cualificador del límite. Cada estado se ve distinto: es el punto de P03. */
static const char *limit_qualifier(ui_limit_state_t s)
{
    switch (s) {
    case UI_LIMIT_KNOWN:      return "";
    case UI_LIMIT_LAST_KNOWN: return "ultimo";
    case UI_LIMIT_INFERRED:   return "inferido";
    case UI_LIMIT_EXPIRED:    return "vencido";
    case UI_LIMIT_UNKNOWN:
    default:                  return "sin dato";
    }
}

/* El límite se muestra como número sólo si es atribuible a algo. Vencido y
 * desconocido esconden el número: presentarlo sería afirmar lo que no se sostiene. */
static bool limit_is_showable(ui_limit_state_t s)
{
    return s == UI_LIMIT_KNOWN || s == UI_LIMIT_LAST_KNOWN || s == UI_LIMIT_INFERRED;
}

/* Una función no disponible se dice, no se simula. */
static const char *status_text(const ui_model_t *m)
{
    switch (m->screen) {
    case UI_SCREEN_WAITING_LINK: return "Esperando tracker";
    case UI_SCREEN_NO_FIX:       return "Sin senal GPS";
    case UI_SCREEN_NO_MAPS:      return "Sin mapas";
    case UI_SCREEN_DRIVING:
    default:                     return NULL;
    }
}

static void ring_opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_INDICATOR);
}

/* Idempotente: llamar con el mismo estado no reinicia la animación. Sin tomar el
 * mutex de LVGL, porque el llamador ya lo tiene y no es recursivo. */
static void apply_overspeed_ring(bool overspeed)
{
    if (!objects.overspeed_ring || overspeed == s_ring_blinking) {
        return;
    }
    s_ring_blinking = overspeed;

    lv_anim_del(objects.overspeed_ring, ring_opa_anim_cb);
    if (overspeed) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.overspeed_ring);
        lv_anim_set_exec_cb(&a, ring_opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, OVERSPEED_RING_BLINK_MS);
        lv_anim_set_playback_time(&a, OVERSPEED_RING_BLINK_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_obj_set_style_arc_opa(objects.overspeed_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }
}

void ui_presenter_tick(void)
{
    if (!s_box_mux) {
        return;
    }

    ui_model_t m;
    bool have = false;
    if (xSemaphoreTake(s_box_mux, 0) == pdTRUE) {
        if (s_has_pending) {
            m = s_pending;
            s_has_pending = false;
            have = true;
        }
        xSemaphoreGive(s_box_mux);
    }
    if (!have) {
        return;
    }
    /* Nada cambió: no se repinta. Un repintado inútil cuesta un flush completo. */
    if (s_has_applied && !model_differs(&m, &s_applied)) {
        return;
    }

    /* PRECONDICIÓN: se ejecuta en la tarea de LVGL con lvgl_lock ya tomado. El
     * mutex de LVGL no es recursivo, así que acá no se vuelve a tomar. */

    /* Números: se hablan por las variables de flow, que es quien las pinta. */
    set_var_current_speed_value(m.speed_state == UI_SPEED_UNKNOWN ? 0 : m.speed_kmh);
    set_var_speed_limit_value(limit_is_showable(m.limit_state) ? m.limit_kmh : 0);

    /* Contenedor del límite: visible sólo cuando hay un número defendible. */
    if (objects.speed_limit_container) {
        if (limit_is_showable(m.limit_state)) {
            lv_obj_clear_flag(objects.speed_limit_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(objects.speed_limit_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (objects.speed_limit_warning_label) {
        lv_label_set_text(objects.speed_limit_warning_label, limit_qualifier(m.limit_state));
    }

    if (objects.street_name) {
        const char *status = status_text(&m);
        if (status) {
            lv_label_set_text(objects.street_name, status);
        } else if (m.show_street) {
            lv_label_set_text(objects.street_name, m.street);
        } else {
            lv_label_set_text(objects.street_name, "");
        }
    }

    apply_overspeed_ring(m.overspeed);

    s_applied = m;
    s_has_applied = true;

    ESP_LOGD(TAG, "pantalla=%s v=%s(%" PRId32 ") limite=%s(%" PRId32 ") exceso=%d",
             ui_screen_name(m.screen),
             ui_speed_state_name(m.speed_state), m.speed_kmh,
             ui_limit_state_name(m.limit_state), m.limit_kmh,
             (int)m.overspeed);
}
