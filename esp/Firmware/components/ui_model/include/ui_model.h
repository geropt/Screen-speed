/* Modelo de presentación del HUD: lógica pura, sin LVGL.
 *
 * Problema que resuelve. Hasta P02b la decisión de qué mostrar estaba repartida
 * entre el bucle principal (`offline_maps.c`), globales sueltas (`vars.c`) y
 * código visual (`dynamic.c`). Tres consecuencias concretas:
 *
 *  - `vars.c` arrancaba con `speed_limit = 78`, un límite inventado que la
 *    pantalla mostraba antes de haber leído un solo tile. Es el «límite inicial
 *    ficticio» que el plan manda retirar.
 *  - La tolerancia y la alerta de exceso vivían dentro de `calculate_threshold()`,
 *    junto a la animación del anillo, así que no se podían probar sin una pantalla.
 *  - No había forma de distinguir en pantalla «todavía no sé» de «lo sabía y
 *    venció»: un cero y un valor viejo se veían igual.
 *
 * Este componente decide **qué** mostrar; no sabe **cómo** dibujarlo. Recibe un
 * `vehicle_snapshot_t` más el resultado del matcher y produce un `ui_model_t`: un
 * struct plano que el presenter del firmware traduce a widgets. Sin LVGL, sin
 * ESP-IDF, sin memoria dinámica, sin locks y con el reloj por parámetro, igual que
 * `vehicle_state`.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vehicle_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STREET_MAX 128

/** Cómo hay que interpretar el límite que se muestra. */
typedef enum {
    UI_LIMIT_UNKNOWN = 0,  /**< nunca se obtuvo un límite: no mostrar número   */
    UI_LIMIT_KNOWN,        /**< límite del tramo actual, recién resuelto        */
    UI_LIMIT_LAST_KNOWN,   /**< último resuelto; la posición ya no lo confirma  */
    UI_LIMIT_INFERRED,     /**< no hay dato de vía; es una inferencia declarada */
    UI_LIMIT_EXPIRED       /**< se tenía, pero hace demasiado para presentarlo  */
} ui_limit_state_t;

/** Cómo hay que interpretar la velocidad que se muestra. */
typedef enum {
    UI_SPEED_UNKNOWN = 0,  /**< sin fix válido todavía                          */
    UI_SPEED_LIVE,         /**< fix fresco                                       */
    UI_SPEED_STALE         /**< el fix venció: el número ya no es actual          */
} ui_speed_state_t;

/** Estado global visible, para elegir pantalla o mensaje. */
typedef enum {
    UI_SCREEN_WAITING_LINK = 0, /**< nunca llegó nada del tracker      */
    UI_SCREEN_DRIVING,          /**< operación normal                   */
    UI_SCREEN_NO_FIX,           /**< había enlace y se perdió el fix    */
    UI_SCREEN_NO_MAPS           /**< sin tarjeta o sin mapas usables    */
} ui_screen_t;

/** Parámetros de presentación. Todos negociables; ninguno medido todavía. */
typedef struct {
    /* Exceso de velocidad con histéresis: se enciende al superar
     * `limit + over_on_kmh` y se apaga al bajar de `limit + over_off_kmh`. Dos
     * umbrales distintos evitan que la alerta titile cuando la velocidad oscila
     * alrededor del límite. */
    int32_t  over_on_kmh;
    int32_t  over_off_kmh;
    /* Cuánto se sigue mostrando un límite ya resuelto cuando el matcher deja de
     * confirmarlo. Pasado esto se declara vencido en lugar de seguir presentándolo
     * como si fuera del tramo actual. */
    uint32_t limit_last_known_ttl_ms;
    /* Velocidad por debajo de la cual no se evalúa exceso: a paso de hombre el
     * rumbo y la velocidad del GPS son poco confiables. */
    int32_t  alert_min_speed_kmh;
} ui_model_config_t;

#define UI_MODEL_CONFIG_DEFAULT() {   \
    .over_on_kmh = 5,                 \
    .over_off_kmh = 2,                \
    .limit_last_known_ttl_ms = 30000, \
    .alert_min_speed_kmh = 5,         \
}

/** Resultado del matcher, tal como lo entrega quien consulta los mapas. */
typedef struct {
    bool     matched;                 /**< hubo tramo resuelto             */
    bool     has_limit;               /**< el tramo declara límite (>0)    */
    int32_t  limit_kmh;
    bool     anonymous;               /**< vía sin nombre                  */
    char     street[UI_STREET_MAX];
    uint64_t tracker_epoch;           /**< época en que se calculó         */
} ui_match_input_t;

/** Lo que hay que mostrar. Struct plano: el presenter sólo copia a widgets. */
typedef struct {
    ui_screen_t      screen;

    ui_speed_state_t speed_state;
    int32_t          speed_kmh;       /**< válido si speed_state != UNKNOWN */

    ui_limit_state_t limit_state;
    int32_t          limit_kmh;       /**< válido si limit_state == KNOWN,
                                           LAST_KNOWN o INFERRED             */

    bool             show_street;
    char             street[UI_STREET_MAX];

    bool             overspeed;       /**< alerta activa, ya con histéresis  */

    bool             sd_present;
    bool             ignition_known;
    bool             ignition_on;
} ui_model_t;

/** Estado interno del presenter. Dueño único; sin locks. */
typedef struct {
    ui_model_config_t cfg;

    bool     overspeed_latched;       /**< para la histéresis                */

    bool     limit_seen;
    int32_t  limit_kmh;
    bool     limit_anonymous;
    uint64_t limit_mono_ms;
    uint64_t limit_epoch;
    char     street[UI_STREET_MAX];
    bool     street_known;

    bool     link_seen;               /**< alguna vez llegó algo del tracker */
} ui_model_state_t;

/** Inicializa. `cfg` NULL usa UI_MODEL_CONFIG_DEFAULT. */
void ui_model_init(ui_model_state_t *st, const ui_model_config_t *cfg);

/**
 * @brief Incorpora un resultado del matcher.
 *
 * Un resultado de una época distinta de la actual se descarta: pertenece a otro
 * tracker. Se pasa `now_ms` para fechar el resultado.
 */
void ui_model_on_match(ui_model_state_t *st, uint64_t now_ms,
                      const ui_match_input_t *in, uint64_t current_epoch);

/**
 * @brief Marca que el matcher corrió y no resolvió tramo.
 *
 * No borra el último límite conocido —el cliente pidió conservarlo— pero permite
 * que envejezca hasta declararse vencido.
 */
void ui_model_on_no_match(ui_model_state_t *st, uint64_t now_ms);

/** Descarta lo acumulado: cambió el tracker o el mapa. */
void ui_model_invalidate(ui_model_state_t *st);

/**
 * @brief Construye lo que hay que mostrar.
 *
 * @param snap        estado del vehículo, con sus edades ya calculadas
 * @param sd_present  si hay tarjeta montada
 * @param out         modelo resultante
 */
void ui_model_build(ui_model_state_t *st, const vehicle_snapshot_t *snap,
                   bool sd_present, ui_model_t *out);

/** Textos cortos para logs y pruebas. */
const char *ui_limit_state_name(ui_limit_state_t s);
const char *ui_speed_state_name(ui_speed_state_t s);
const char *ui_screen_name(ui_screen_t s);

#ifdef __cplusplus
}
#endif
