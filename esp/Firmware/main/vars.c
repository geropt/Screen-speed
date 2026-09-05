#include "ui/vars.h"
#include "dynamic.h"

int32_t indicator_threshold = 10;
int32_t current_speed = 0;
/* P03: retirado el límite inicial ficticio.
 *
 * Esto valía 78, así que la pantalla mostraba «78 km/h» desde el arranque, antes
 * de haber leído un solo tile y sin saber en qué calle está el vehículo. Ahora
 * arranca en 0, y el presenter esconde el contenedor del límite mientras el estado
 * sea desconocido o vencido: nunca se muestra un número que no se pueda defender.
 * Quién decide eso es components/ui_model, con pruebas. */
int32_t speed_limit = 0;


int32_t get_var_indicator_threshold_value()
{
    return indicator_threshold;
}

void set_var_indicator_threshold_value(int32_t value)
{
    indicator_threshold = value;
}

int32_t get_var_current_speed_value()
{
    return current_speed;
}

void set_var_current_speed_value(int32_t value)
{
    current_speed = value;
}

int32_t get_var_speed_limit_value()
{
    return speed_limit;
}

void set_var_speed_limit_value(int32_t value)
{
    speed_limit = value;
}
