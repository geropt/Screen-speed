#ifndef _RGB_LCD_H_
#define _RGB_LCD_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board_waveshare_175.h"
#include "board_i2c.h"

/* P01: los pines, la geometría del panel y la secuencia de inicialización se
 * movieron a components/board_waveshare_175 sin cambiar ningún valor. Este
 * archivo queda como el puerto de LVGL: buffers, callbacks, tarea y mutex.
 *
 * Se conservan los alias LCD_H_RES / LCD_V_RES porque main/splash.c los usa; son
 * el mismo número, no una segunda definición. */

#define RUN_APPLICATION_UI      1

#define LCD_HOST        BOARD_LCD_SPI_HOST

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL (16)
#endif

#define LCD_H_RES       BOARD_LCD_H_RES
#define LCD_V_RES       BOARD_LCD_V_RES

/* Filas por buffer de dibujo. Antes era LCD_V_RES / 4 fijo (116); ahora sale de
 * menuconfig con 116 como default, así que la línea base no cambia. Ver
 * components/board_waveshare_175/Kconfig para el costo de cada opción. */
#define LVGL_BUF_HEIGHT         BOARD_LVGL_BUF_ROWS

#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  1
#define LVGL_TASK_STACK_SIZE    (4 * 1024)
#define LVGL_TASK_PRIORITY      2

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);


esp_err_t waveshare_led_init();

#ifdef __cplusplus
}
#endif

#endif
