#include "nmea_parser.h"
#include "telematics.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pilot";

void app_main(void)
{
    ESP_LOGI(TAG, "starlink pilot: UART Ruptela + cmd 68");

    telematics_start();

    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    config.uart.uart_port = UART_NUM_2;
#endif
    nmea_parser_handle_t nmea = nmea_parser_init(&config);
    if (nmea == NULL) {
        ESP_LOGE(TAG, "nmea_parser_init failed");
        return;
    }
    nmea_parser_add_handler(nmea, telematics_on_nmea, nmea);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
