#include "sd_manager.h"
#include "tile_reader.h"
#include "waveshare_amoled_lcd_port.h"
#include "dynamic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nmea_parser.h"

#define TIME_ZONE (+8)   // Beijing Time
#define YEAR_BASE (2000) // date in GPS starts from 2000

static const char *TAG = "MAIN";
static QueueHandle_t gps_queue;

static void calculation_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // initial delay

    while (1)
    {
        calculate_threshold();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    gps_t *gps = NULL;
    switch (event_id)
    {
    case GPS_UPDATE:
        gps = (gps_t *)event_data;
        /* print information parsed from GPS statements */
        ESP_LOGI(TAG, "%d/%d/%d %d:%d:%d => \r\n"
                      "\t\t\t\t\t\tlatitude   = %.05f°N\r\n"
                      "\t\t\t\t\t\tlongitude = %.05f°E\r\n"
                      "\t\t\t\t\t\taltitude   = %.02fm\r\n"
                      "\t\t\t\t\t\tspeed      = %fm/s",
                 gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
                 gps->tim.hour + TIME_ZONE, gps->tim.minute, gps->tim.second,
                 gps->latitude, gps->longitude, gps->altitude, gps->speed);

        gps_t gps_copy;
        memcpy(&gps_copy, event_data, sizeof(gps_t));
        xQueueSendFromISR(gps_queue, &gps_copy, NULL);
        break;
    case GPS_UNKNOWN:
        /* print unknown statements */
        ESP_LOGW(TAG, "Unknown statement:%s", (char *)event_data);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t err;

    waveshare_led_init();

    err = sd_card_init();
    if (err != ESP_OK)
        return;

    xTaskCreate(calculation_task, "calculation_task", 4096, NULL, 5, NULL);
    gps_queue = xQueueCreate(5, sizeof(gps_t));

    /* NMEA parser configuration */
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    config.uart.baud_rate = 115200;
    /* init NMEA parser library */
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&config);
    /* register event handler for NMEA parser library */
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);

    while (1)
    {
        gps_t gps;
        if (xQueueReceive(gps_queue, &gps, portMAX_DELAY))
        {
            static int speed_limit;
            static char street[128];
            if (get_speed_and_name_at(gps.latitude, gps.longitude, &speed_limit, street, sizeof(street)))
            {
                ESP_LOGI(TAG, "Speed limit: %d km/h\n", speed_limit);
                ESP_LOGI(TAG, "Street: %s\n", street);
                set_street_name(street);
                set_var_speed_limit_value(speed_limit);
            }
            else
            {
                ESP_LOGI(TAG, "No data for this location.");
                // set_var_speed_limit_value(0);    // removed as per client's request, retain last known speed limit value
            }
            int32_t current_speed = (int32_t)(gps.speed * 3.6f); // m/s to km/h
            ESP_LOGI(TAG, "Current Speed: %ld km/h\n", current_speed);
            set_var_current_speed_value(current_speed);
        }
    }
}
