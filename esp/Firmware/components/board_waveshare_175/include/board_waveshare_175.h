/* Servicios de placa: Waveshare ESP32-S3-Touch-AMOLED-1.75.
 *
 * Extraído en P01 desde main/waveshare_amoled_lcd_port.h sin cambiar ningún
 * valor: los pines, la geometría y la secuencia de inicialización del panel son
 * los mismos que la línea base P00. El objetivo es tener un único lugar donde
 * describir la placa, no mejorar todavía cómo se usa.
 *
 * ADVERTENCIA de hardware sin verificar (ver docs/baseline-P00.md, sección 7):
 *
 *  - El fabricante documenta el panel como CO5300 y aquí se usa el driver
 *    SH8601 con la secuencia de comandos que funciona hoy. No cambiar de driver
 *    por el nombre: exige comparación visual propia.
 *  - El touch documentado es CST9217, el proyecto trae dependencia FT5x06 y el
 *    código deshabilitado usa TouchDrvCST92xx de SensorLib. Sin resolver.
 *  - GPIO18 se usa como UART1 RX del Ruptela. En la variante -G esa expansión
 *    comparte camino con el GNSS LC76G. Verificar la revisión física antes de
 *    dar el pin por libre.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_sh8601.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Identidad de la placa ---------- */

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-AMOLED-1.75"

/* ---------- Panel ---------- */

#define BOARD_LCD_H_RES         466
#define BOARD_LCD_V_RES         466

#define BOARD_LCD_SPI_HOST      SPI2_HOST

#define BOARD_PIN_LCD_CS        (GPIO_NUM_12)
#define BOARD_PIN_LCD_PCLK      (GPIO_NUM_38)
#define BOARD_PIN_LCD_DATA0     (GPIO_NUM_4)
#define BOARD_PIN_LCD_DATA1     (GPIO_NUM_5)
#define BOARD_PIN_LCD_DATA2     (GPIO_NUM_6)
#define BOARD_PIN_LCD_DATA3     (GPIO_NUM_7)
#define BOARD_PIN_LCD_RST       (GPIO_NUM_39)
/* Sin pin de backlight: el AMOLED regula brillo por comando (0x51). */
#define BOARD_PIN_BK_LIGHT      (-1)
#define BOARD_BK_LIGHT_ON_LEVEL  1
#define BOARD_BK_LIGHT_OFF_LEVEL (!BOARD_BK_LIGHT_ON_LEVEL)

/**
 * @brief Secuencia de inicialización del panel usada por la línea base.
 *
 * Se expone como función y no como arreglo global para que quede un solo dueño
 * de la secuencia. Copiada byte por byte de la versión que funciona; cualquier
 * cambio necesita su propia comparación visual en la placa.
 *
 * @param out_count recibe la cantidad de comandos
 * @return puntero a la secuencia, válido durante toda la vida del programa
 */
const sh8601_lcd_init_cmd_t *board_lcd_init_cmds(size_t *out_count);

/* ---------- Bus I2C compartido (touch / PMIC / RTC) ---------- */

#define BOARD_PIN_I2C_SDA       (GPIO_NUM_15)
#define BOARD_PIN_I2C_SCL       (GPIO_NUM_14)
#define BOARD_I2C_FREQ_HZ       100000

/* ---------- Touch ---------- */

#define BOARD_PIN_TOUCH_RST     (GPIO_NUM_40)
#define BOARD_PIN_TOUCH_INT     (GPIO_NUM_11)
#define BOARD_TOUCH_I2C_ADDR    0x5A

/* ---------- UART del Ruptela ---------- */

/* El pin efectivo lo fija CONFIG_NMEA_PARSER_UART_RXD (18 en la referencia).
 * Se declara aquí para que el mapa de la placa quede completo en un solo lugar. */
#define BOARD_PIN_RUPTELA_RX    (CONFIG_NMEA_PARSER_UART_RXD)

/* ---------- Tarjeta SD ---------- */
/* Los pines viven en components/sd_card/sd_manager.h y no se mueven en P01
 * para no tocar el montaje de la tarjeta en el mismo cambio. */

/* ---------- Buffers de dibujo ---------- */

/**
 * @brief Filas por buffer de dibujo de LVGL, configurable en menuconfig.
 *
 * La línea base P00 usa 466/4 = 116 filas en dos buffers `MALLOC_CAP_DMA`, es
 * decir 2 × 466 × 116 × 2 B = 216 224 B de RAM interna. Bajar a 32 filas libera
 * 156 576 B (152,91 KiB). Ese ahorro es aritmética del tamaño del buffer, **no**
 * una medición de heap ni una comprobación de que la UI siga fluida: hay que
 * comparar en la placa con la misma UI y el mismo replay.
 */
#define BOARD_LVGL_BUF_ROWS     (CONFIG_BOARD_LVGL_BUF_ROWS)

/** Bytes por buffer de dibujo, con la profundidad de color configurada. */
#define BOARD_LVGL_BUF_BYTES \
    ((size_t)BOARD_LCD_H_RES * BOARD_LVGL_BUF_ROWS * (CONFIG_LV_COLOR_DEPTH / 8))

/* ---------- Capacidades declaradas ---------- */

/**
 * @brief Lo que el firmware cree de la placa, para dejarlo en el log de arranque.
 *
 * Son valores declarados por configuración y por la documentación del
 * fabricante, no leídos del chip. `board_log_identity()` los imprime una vez
 * junto a lo que sí se puede consultar en tiempo de ejecución, para que un log
 * de campo permita detectar una unidad distinta de la esperada.
 */
void board_log_identity(void);

#ifdef __cplusplus
}
#endif
