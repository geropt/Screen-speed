#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pantalla de arranque con el logo de mykeego y una barra de progreso.
// Todas las funciones toman internamente lvgl_lock(), por lo que se pueden
// llamar de forma segura desde app_main / la tarea de calculo.

// Crea y carga la pantalla de splash (logo + barra al 0%). Debe llamarse
// despues de ui_init() (necesita objects.main como destino de la transicion).
void splash_show(void);

// Actualiza el anillo de progreso (0..100).
void splash_set_progress(uint8_t pct);

// Lleva la barra al 100%, hace fade hacia la pantalla principal y libera el
// splash. Idempotente: llamadas posteriores no hacen nada.
void splash_finish(void);

#ifdef __cplusplus
}
#endif

#endif // SPLASH_H
