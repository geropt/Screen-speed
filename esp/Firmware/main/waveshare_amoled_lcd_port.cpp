#include "waveshare_amoled_lcd_port.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "esp_lcd_sh8601.h"
#include "board_waveshare_175.h"
#include "board_i2c.h"
#include "res_metrics.h"
#include "ui_presenter.h"
#include "ui/ui.h"
#include "splash.h"

#if CONFIG_BOARD_ENABLE_TOUCH
#include "driver/i2c.h"
#include "SensorLib.h"
#include "TouchDrvCST92xx.h"
#endif


const static char *TAG = "LCD_PORT";

static SemaphoreHandle_t lvgl_mux = NULL;

#if CONFIG_BOARD_ENABLE_TOUCH
TouchDrvCST92xx touch;
int16_t x[5], y[5];
bool isPressed = false;
#endif

/* La secuencia de inicialización del panel vive ahora en
 * components/board_waveshare_175, sin cambios respecto de la línea base. */

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BOARD_ENABLE_TOUCH
/* El bus lo instala board_i2c, que es el único dueño: el touch comparte SDA/SCL
 * con PMIC y RTC. Si el controlador no responde, se sigue sin touch: el HUD debe
 * mostrar velocidad igual. */
static bool setup_touch(void)
{
    esp_err_t err = board_i2c_acquire();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sin bus I2C (%s): touch deshabilitado", esp_err_to_name(err));
        res_metrics_error(RES_ERR_I2C_BUS);
        return false;
    }

    err = board_i2c_probe(BOARD_TOUCH_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch 0x%02x no responde (%s): se sigue sin touch",
                 BOARD_TOUCH_I2C_ADDR, esp_err_to_name(err));
        return false;
    }

    touch.setPins(BOARD_PIN_TOUCH_RST, BOARD_PIN_TOUCH_INT);
    /* Variante por callbacks: el driver no instala ni configura el bus, sólo lee
     * y escribe registros a través de board_i2c. La variante
     * begin(i2c_port_t, ...) de SensorLib llama a i2c_driver_install() por su
     * cuenta y rompería el dueño único. */
    if (!touch.begin(BOARD_TOUCH_I2C_ADDR, board_i2c_read_regs, board_i2c_write_regs)) {
        ESP_LOGE(TAG, "touch.begin() falló: se sigue sin touch");
        return false;
    }
    touch.reset();
    touch.setMaxCoordinates(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    touch.setMirrorXY(true, true);
    ESP_LOGI(TAG, "touch listo");
    return true;
}
#endif

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    /* Contexto de ISR del SPI. Sólo aritmética entera y esp_timer_get_time(),
     * que es segura desde ISR; nada de logs ni de asignaciones. La función
     * tampoco se marca IRAM_ATTR: en la línea base la IRAM está a 1 byte del
     * límite, y este callback ya llamaba a lv_disp_flush_ready(), que vive en
     * flash. */
    res_metrics_end(RES_CH_LCD_FLUSH);
    /* Cierra la traza recepción→snapshot→flush. No toca widgets: es la única
     * excepción admitida a «la UI es el único escritor». */
    ui_presenter_note_flush_done();
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1; //+ 0x16;
    const int offsetx2 = area->x2; //+ 0x16;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

#if LCD_BIT_PER_PIXEL == 24
    uint8_t *to = (uint8_t *)color_map;
    uint8_t temp = 0;
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);

    // Special dealing for first pixel
    temp = color_map[0].ch.blue;
    *to++ = color_map[0].ch.red;
    *to++ = color_map[0].ch.green;
    *to++ = temp;
    // Normal dealing for other pixels
    for (int i = 1; i < pixel_num; i++)
    {
        *to++ = color_map[i].ch.red;
        *to++ = color_map[i].ch.green;
        *to++ = color_map[i].ch.blue;
    }
#endif

    // copy a buffer's content to a specific area of the display
    res_metrics_begin(RES_CH_LCD_FLUSH);
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_update_cb(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;

    switch (drv->rotated)
    {
    case LV_DISP_ROT_NONE:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISP_ROT_90:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISP_ROT_180:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISP_ROT_270:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}

void lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

#if CONFIG_BOARD_ENABLE_TOUCH
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint8_t touched = touch.getPoint(x, y, 2);
    if (touched)
    {

        for (int i = 0; i < 1; ++i)
        {
            data->point.x = x[0];
            data->point.y = y[0];
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "Touch[%d]: X=%d Y=%d", i, x[i], y[i]);
        }
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    res_metrics_watch_task("lvgl", NULL);
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1)
    {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (lvgl_lock(-1))
        {
            res_metrics_begin(RES_CH_LVGL_TICK);
            task_delay_ms = lv_timer_handler();
            /* P03: el presenter es el único escritor de widgets y corre acá, con
             * el mutex ya tomado. Va ANTES de ui_tick() porque publica las
             * variables de flow que el código generado por EEZ pinta enseguida. */
            ui_presenter_tick();
            ui_tick();
            res_metrics_end(RES_CH_LVGL_TICK);
            // Release the mutex
            lvgl_unlock();
        }
        /* Resumen periódico de recursos. Va acá y no en una tarea propia: esta
         * ya corre siempre, y la compuerta interna limita la impresión a una vez
         * por período. */
        res_metrics_log_due();
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }
        else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

esp_err_t waveshare_led_init()
{
    esp_err_t err = ESP_OK;
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    res_metrics_init();
    board_log_identity();

#if BOARD_PIN_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_PIN_BK_LIGHT};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = BOARD_PIN_LCD_PCLK;
    buscfg.data0_io_num = BOARD_PIN_LCD_DATA0;
    buscfg.data1_io_num = BOARD_PIN_LCD_DATA1;
    buscfg.data2_io_num = BOARD_PIN_LCD_DATA2;
    buscfg.data3_io_num = BOARD_PIN_LCD_DATA3;
    buscfg.max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t);
    buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(BOARD_PIN_LCD_CS,
                                                                                notify_lvgl_flush_ready,
                                                                                &disp_drv);
    size_t init_cmds_count = 0;
    const sh8601_lcd_init_cmd_t *init_cmds = board_lcd_init_cmds(&init_cmds_count);
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = init_cmds,
        .init_cmds_size = (uint16_t)init_cmds_count,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_LOGI(TAG, "Install SH8601 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_BOARD_ENABLE_TOUCH
    bool touch_ready = setup_touch();
#endif

#if BOARD_PIN_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(BOARD_PIN_BK_LIGHT, BOARD_BK_LIGHT_ON_LEVEL);
#endif

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    /* Dos buffers de dibujo en RAM interna capaz de DMA. La cantidad de filas
     * sale de menuconfig (116 en la línea base). Un fallo acá no puede pasar
     * silenciosamente: se cuenta y se registra el tamaño pedido antes del
     * assert, para que un log de campo diga cuánto faltaba. */
    const size_t buf_bytes = (size_t)BOARD_LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t);
    ESP_LOGI(TAG, "buffers de dibujo: %d filas, %u B cada uno (%u B en total)",
             (int)LVGL_BUF_HEIGHT, (unsigned)buf_bytes, (unsigned)(buf_bytes * 2));
    lv_color_t *buf1 = static_cast<lv_color_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA));
    lv_color_t *buf2 = static_cast<lv_color_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA));
    if (!buf1 || !buf2) {
        res_metrics_error(RES_ERR_ALLOC_FAILED);
        ESP_LOGE(TAG, "sin RAM interna para los buffers de dibujo (%u B x2); "
                      "mayor bloque DMA libre: %u B",
                 (unsigned)buf_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    assert(buf1);
    assert(buf2);
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, BOARD_LCD_H_RES * LVGL_BUF_HEIGHT);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BOARD_LCD_H_RES;
    disp_drv.ver_res = BOARD_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.drv_update_cb = lvgl_update_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

#if CONFIG_BOARD_ENABLE_TOUCH
    if (touch_ready) {
        static lv_indev_drv_t indev_drv; // Input device driver (Touch)
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = lvgl_touch_cb;
        indev_drv.user_data = &touch;
        lv_indev_drv_register(&indev_drv);
    }
#else
    /* Sin touch no hay dispositivo de entrada que registrar; `disp` queda sin uso
     * y el compilador lo señalaría con -Wunused-variable. */
    (void)disp;
#endif

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    if (ui_presenter_init() != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo crear el buzon del presenter");
        res_metrics_error(RES_ERR_ALLOC_FAILED);
    }
    
    ESP_LOGI(TAG, "Display LVGL demos");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (lvgl_lock(-1))
    {
#if RUN_APPLICATION_UI
        ui_init();
#elif LV_USE_DEMO_WIDGETS
        lv_demo_widgets();      /* A widgets example */
#elif LV_USE_DEMO_MUSIC
        lv_demo_music(); /* A modern, smartphone-like music player demo. */
#elif LV_USE_DEMO_STRESS
        lv_demo_stress();       /* A stress test for LVGL. */
#elif LV_USE_DEMO_BENCHMARK
        lv_demo_benchmark();    /* A demo to measure the performance of LVGL or to compare different settings. */
#endif
        // Release the mutex
        lvgl_unlock();
    }

#if RUN_APPLICATION_UI
    // Carga el splash (logo mykeego + barra) encima de la pantalla Main, visible
    // desde el primer frame mientras app_main inicializa el resto. splash_show()
    // toma lvgl_lock() por su cuenta, por eso va FUERA del bloque de arriba (el
    // mutex no es recursivo).
    splash_show();
#endif

    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    return err;
}

#ifdef __cplusplus
} /*extern "C"*/
#endif
