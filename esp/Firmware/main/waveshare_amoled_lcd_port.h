#ifndef _RGB_LCD_H_
#define _RGB_LCD_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define RUN_APPLICATION_UI      1

#define LCD_HOST        SPI2_HOST
#define TOUCH_HOST      I2C_NUM_0

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL (16)
#endif

#define LCD_BK_LIGHT_ON_LEVEL   1
#define LCD_BK_LIGHT_OFF_LEVEL  !LCD_BK_LIGHT_ON_LEVEL
#define PIN_NUM_LCD_CS          (GPIO_NUM_12)
#define PIN_NUM_LCD_PCLK        (GPIO_NUM_38)
#define PIN_NUM_LCD_DATA0       (GPIO_NUM_4)
#define PIN_NUM_LCD_DATA1       (GPIO_NUM_5)
#define PIN_NUM_LCD_DATA2       (GPIO_NUM_6)
#define PIN_NUM_LCD_DATA3       (GPIO_NUM_7)
#define PIN_NUM_LCD_RST         (GPIO_NUM_39)
#define PIN_NUM_BK_LIGHT        (-1)

// The pixel number in horizontal and vertical
#define LCD_H_RES 466
#define LCD_V_RES 466

#define USE_TOUCH 0

#if USE_TOUCH
#define PIN_NUM_TOUCH_SCL (GPIO_NUM_14)
#define PIN_NUM_TOUCH_SDA (GPIO_NUM_15)
#define PIN_NUM_TOUCH_RST (GPIO_NUM_40)
#define PIN_NUM_TOUCH_INT (GPIO_NUM_11)

#define I2C_MASTER_NUM (i2c_port_t)1
#define I2C_MASTER_FREQ_HZ 100000 /*!< I2C master clock frequency */
#define I2C_MASTER_SDA_IO (gpio_num_t)15
#define I2C_MASTER_SCL_IO (gpio_num_t)14
#define Touch_INT (gpio_num_t)11
#define Touch_RST (gpio_num_t)40

#define I2C_MASTER_TX_BUF_DISABLE   0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000
#define TOUCH_SENSOR_ADDR           0x5A;

#endif


// V_RES/8 (era /4): dos draw buffers de este alto en RAM interna DMA deben
// entrar junto al stack WiFi. En interna (no PSRAM) el flush por DMA es inmune
// al cache-disable de las escrituras a flash (ver waveshare_led_init()).
#define LVGL_BUF_HEIGHT         (LCD_V_RES / 8)
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