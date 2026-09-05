/* Presenter del HUD: el único escritor de widgets.
 *
 * Regla de P03: **todas** las escrituras de widgets pasan por acá, y ocurren
 * dentro de la tarea de LVGL con el mutex tomado. La única excepción admitida es
 * la señal de flush-ready del puerto de display, que corre en el callback de DMA y
 * no toca widgets.
 *
 * Antes, la tarea principal llamaba a `set_street_name()` y a `set_var_*()`
 * mientras la tarea de LVGL corría `lv_timer_handler()`. Una de esas rutas ni
 * tomaba el mutex.
 *
 * Cómo se ordena ahora:
 *
 *   tarea principal          presenter (tarea LVGL)
 *   ---------------          ----------------------
 *   arma ui_model_t   --->   ui_presenter_publish()   copia bajo mutex propio
 *                            ui_presenter_tick()      escribe widgets con lvgl_lock
 *
 * El modelo se publica por copia en un buzón protegido, así la tarea principal
 * nunca espera a LVGL ni al panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ui_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Crea el buzón. Llamar antes de publicar. */
esp_err_t ui_presenter_init(void);

/**
 * @brief Publica el modelo a mostrar. No bloquea contra LVGL.
 *
 * Se puede llamar desde cualquier tarea. Sobrescribe el modelo anterior: es
 * estado, no una cola de eventos.
 */
void ui_presenter_publish(const ui_model_t *model);

/**
 * @brief Aplica a los widgets el último modelo publicado.
 *
 * Debe llamarse SOLO desde la tarea de LVGL. Toma `lvgl_lock` una vez por
 * invocación y no escribe nada si el modelo no cambió, para no repintar de gusto.
 */
void ui_presenter_tick(void);

/**
 * @brief Marca que se completó un flush del panel.
 *
 * La llama el puerto de display desde el callback de DMA. Sólo cierra la medición
 * de la traza recepción→snapshot→flush; no toca widgets.
 */
void ui_presenter_note_flush_done(void);

/**
 * @brief Registra el instante en que se recibió el fix que originó el modelo.
 *
 * Con esto la traza mide de punta a punta: del fix válido al flush terminado.
 */
void ui_presenter_note_fix_received(uint64_t mono_ms);

#ifdef __cplusplus
}
#endif
