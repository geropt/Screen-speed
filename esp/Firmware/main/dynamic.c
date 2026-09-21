#include "dynamic.h"
#include "screens.h"
#include "eez-flow.h"
#include "waveshare_amoled_lcd_port.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"

/* P03: este archivo ya no decide nada.
 *
 * Antes tenía la tolerancia y la alerta de exceso —`calculate_threshold()` leía
 * las variables de la UI, comparaba y prendía el titileo del anillo— mezcladas con
 * la animación. Eso no se podía probar sin una pantalla, y además convertía al
 * código visual en un segundo escritor de widgets junto a la tarea principal.
 *
 * Ahora:
 *  - la decisión de exceso, con histéresis y con las reglas de qué datos habilitan
 *    una alerta, vive en components/ui_model y tiene pruebas en host;
 *  - la escritura de widgets, incluido el anillo, vive en main/ui_presenter.c y
 *    corre en la tarea de LVGL;
 *  - `set_street_name()` queda para compatibilidad de la firma pública, pero ya
 *    nadie del firmware la usa: el renglón de la calle lo escribe el presenter.
 */

static const char *TAG = "DYNAMIC";

void set_street_name(const char *street_name)
{
    /* Ruta obsoleta. Escribir el widget desde acá volvería a introducir un segundo
     * escritor fuera de la tarea de LVGL, que es lo que P03 eliminó. Se deja el
     * símbolo para no romper compilaciones ajenas y se avisa una vez. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "set_street_name() está obsoleta: la calle la escribe ui_presenter");
    }
    (void)street_name;
}

void calculate_threshold(void)
{
    /* Obsoleta por la misma razón: la decisión de exceso ahora es lógica pura en
     * components/ui_model, evaluada con el estado del vehículo y sus edades. */
}
