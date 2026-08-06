#include "sd_manager.h"
#include "tile_reader.h"
#include "tile_cache.h"
#include "esp_heap_caps.h"
#include "waveshare_amoled_lcd_port.h"
#include "dynamic.h"
#include "splash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <inttypes.h>
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
        {
            /* Log the baud rate the auto-probe locked onto, once, so it
             * ends up in /sdcard/diag.log for each unit. */
            static bool baud_logged = false;
            if (!baud_logged) {
                ESP_LOGI(TAG, "NMEA link up at %" PRIu32 " baud", nmea_parser_get_baud(event_handler_arg));
                baud_logged = true;
            }
        }
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
    // El splash (logo mykeego + anillo) ya quedo cargado dentro de waveshare_led_init().

    err = sd_card_init();
    if (err != ESP_OK)
        return;
    splash_set_progress(40);

    xTaskCreate(calculation_task, "calculation_task", 4096, NULL, 5, NULL);
    gps_queue = xQueueCreate(5, sizeof(gps_t));

    /* NMEA parser configuration. Initial baud rate comes from
     * CONFIG_NMEA_PARSER_UART_BAUD_RATE; the parser auto-probes between
     * 9600 and 115200 until it sees NMEA with a valid checksum, so it
     * works with both Pro5-Lite/HCV5-Lite (capped at 9600) and Pro5/HCV5
     * (115200) trackers without reflashing. */
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    /* init NMEA parser library */
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&config);
    /* register event handler for NMEA parser library */
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, nmea_hdl);
    splash_set_progress(90);

    // Inicializacion completa: fundido del splash hacia la pantalla principal.
    splash_finish();

    while (1)
    {
        gps_t gps;
        if (xQueueReceive(gps_queue, &gps, portMAX_DELAY))
        {
            static int speed_limit;
            static char street[128];
            float speed_kmh = gps.speed * 3.6f;
            if (get_speed_and_name_at(gps.latitude, gps.longitude, gps.cog, speed_kmh,
                                      &speed_limit, street, sizeof(street)))
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
            int32_t current_speed = (int32_t)speed_kmh;
            ESP_LOGI(TAG, "Current Speed: %" PRId32 " km/h\n", current_speed);
            set_var_current_speed_value(current_speed);

            /* Tile cache health, once a minute. A warm cache should sit well
             * above 95% hits: a whole trip only touches 42-128 distinct tiles.
             * If `bytes` keeps climbing instead of levelling off, the LRU is not
             * evicting. */
            static uint32_t fix_count;
            if ((++fix_count % 60) == 0) {
                uint32_t hits, misses, entries;
                size_t bytes;
                tile_cache_stats(&hits, &misses, &bytes, &entries);
                uint32_t total = hits + misses;
                ESP_LOGI(TAG, "tiles: %" PRIu32 "%% hits (%" PRIu32 "/%" PRIu32
                              "), %u KB in %" PRIu32 " entries, PSRAM free %u KB",
                         total ? (hits * 100 / total) : 0, hits, total,
                         (unsigned)(bytes / 1024), entries,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            }
        }
    }
}
