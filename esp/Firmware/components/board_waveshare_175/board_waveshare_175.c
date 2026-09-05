#include "board_waveshare_175.h"

#include <stddef.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_psram.h"

static const char *TAG = "BOARD";

/* Secuencia de inicialización del panel tal como la usa la línea base P00.
 * No se reordenó, no se agregaron ni quitaron comandos y se conservaron los
 * retardos. 0x2A/0x2B fijan la ventana activa 466 × 466 con el offset del panel;
 * 0x11 (sleep out) y 0x29 (display on) mantienen sus 600 ms. */
static const sh8601_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

const sh8601_lcd_init_cmd_t *board_lcd_init_cmds(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]);
    }
    return s_lcd_init_cmds;
}

void board_log_identity(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);

    /* Declarado por configuración contra lo que se puede consultar en ejecución.
     * Si estas dos columnas no coinciden, la unidad no es la esperada. */
    ESP_LOGI(TAG, "placa=%s panel=%dx%d driver=SH8601 (fabricante documenta CO5300)",
             BOARD_NAME, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    /* `revision` viene como mayor*100 + menor en IDF 5.x. */
    ESP_LOGI(TAG, "chip: cores=%d rev=%d.%d features=%s%s",
             chip.cores, chip.revision / 100, chip.revision % 100,
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
             (chip.features & CHIP_FEATURE_BLE) ? "ble" : "");
    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "flash: %" PRIu32 " KiB leídos del chip (config declara %s)",
                 flash_size / 1024, CONFIG_ESPTOOLPY_FLASHSIZE);
    } else {
        ESP_LOGW(TAG, "flash: no se pudo leer el tamaño (%s); config declara %s",
                 esp_err_to_name(flash_err), CONFIG_ESPTOOLPY_FLASHSIZE);
    }
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "psram: %u KiB detectados", (unsigned)(esp_psram_get_size() / 1024));
#else
    ESP_LOGW(TAG, "psram: deshabilitada en configuración");
#endif
    ESP_LOGI(TAG, "buffers LVGL: %d filas × 2 = %u B internos (DMA)",
             BOARD_LVGL_BUF_ROWS, (unsigned)(BOARD_LVGL_BUF_BYTES * 2));
    ESP_LOGI(TAG, "uart ruptela: RX GPIO%d", BOARD_PIN_RUPTELA_RX);
}
