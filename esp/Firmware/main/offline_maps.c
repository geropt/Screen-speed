#include "sd_manager.h"
#include "tile_reader.h"
#include "waveshare_amoled_lcd_port.h"
#include "dynamic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nmea_parser.h"
#include "buttons.h"
#include <math.h>
#include <string.h>

#define TIME_ZONE (-3)   // Argentina (UTC-3)
#define YEAR_BASE (2000) // date in GPS starts from 2000

// Discard implausible ground speeds (corrupt/invalid NMEA) instead of showing them.
#define MAX_PLAUSIBLE_SPEED_KMH   250

// Minimum spacing between processed GPS samples. The NMEA parser emits an event
// per sentence (several per second); we only need ~1 Hz for matching/display.
#define GPS_PROCESS_PERIOD_US     900000   // ~0.9 s

// If no valid NMEA arrives for this long, assume the tracker/RS232 link is down.
#define GPS_LINK_TIMEOUT_US       5000000  // 5 s

static const char *TAG = "MAIN";
static QueueHandle_t gps_queue;

// Last time any valid NMEA statement was received (link-alive heartbeat).
// Written from the event task, read from the main loop; coarse timeout only.
static volatile int64_t last_gps_us = 0;

static void calculation_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // initial delay

    while (1)
    {
        calculate_threshold();
        street_message_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    static int64_t last_enqueue_us = 0;
    gps_t *gps = NULL;
    switch (event_id)
    {
    case GPS_UPDATE:
    {
        gps = (gps_t *)event_data;

        int64_t now = esp_timer_get_time();
        last_gps_us = now; // a valid (CRC-checked) statement arrived: link is alive

        /* print information parsed from GPS statements */
        ESP_LOGI(TAG, "%d/%d/%d %d:%d:%d => \r\n"
                      "\t\t\t\t\t\tlatitude   = %.05f°N\r\n"
                      "\t\t\t\t\t\tlongitude = %.05f°E\r\n"
                      "\t\t\t\t\t\taltitude   = %.02fm\r\n"
                      "\t\t\t\t\t\tspeed      = %fm/s",
                 gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
                 gps->tim.hour + TIME_ZONE, gps->tim.minute, gps->tim.second,
                 gps->latitude, gps->longitude, gps->altitude, gps->speed);

        /* Throttle: only forward ~1 sample/sec to the matching/display loop */
        if (now - last_enqueue_us < GPS_PROCESS_PERIOD_US)
            break;
        last_enqueue_us = now;

        gps_t gps_copy;
        memcpy(&gps_copy, event_data, sizeof(gps_t));
        /* Runs in the event-loop task (NOT an ISR): use the regular send. */
        xQueueSend(gps_queue, &gps_copy, 0);
        break;
    }
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

    buttons_init();

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

    bool link_down_shown = false;

    while (1)
    {
        gps_t gps;
        if (xQueueReceive(gps_queue, &gps, pdMS_TO_TICKS(1000)))
        {
            link_down_shown = false;

            /* ---- 1. Require a usable GPS fix before trusting any field ---- */
            bool fix_ok = gps.valid &&
                          gps.fix != GPS_FIX_INVALID &&
                          isfinite(gps.latitude) && isfinite(gps.longitude) &&
                          !((gps.latitude == 0.0f) && (gps.longitude == 0.0f));

            if (!fix_ok)
            {
                ESP_LOGW(TAG, "No GPS fix yet (valid=%d fix=%d)", gps.valid, gps.fix);
                set_street_name("Buscando GPS...");
                set_var_current_speed_value(0);
                continue;
            }

            /* ---- 2. Sanitize speed (reject garbage like 300 km/h) ---- */
            float kmh = gps.speed * 3.6f; // m/s to km/h
            if (isfinite(kmh) && kmh >= 0.0f && kmh <= MAX_PLAUSIBLE_SPEED_KMH)
            {
                int32_t current_speed = (int32_t)kmh;
                ESP_LOGI(TAG, "Current Speed: %ld km/h", current_speed);
                set_var_current_speed_value(current_speed);
            }
            else
            {
                ESP_LOGW(TAG, "Discarding implausible speed: %.1f km/h", kmh);
            }

            /* ---- 3. Map-match street + speed limit ---- */
            static int speed_limit;
            static char street[128];
            if (get_speed_and_name_at(gps.latitude, gps.longitude, &speed_limit, street, sizeof(street)))
            {
                ESP_LOGI(TAG, "Speed limit: %d km/h", speed_limit);
                ESP_LOGI(TAG, "Street: %s", street);
                set_street_name(street);
                // Keep the previous valid limit if this segment is untagged (0).
                if (speed_limit > 0)
                    set_var_speed_limit_value(speed_limit);
            }
            else
            {
                ESP_LOGI(TAG, "No data for this location.");
                // retain last known speed limit value (per client's request)
            }
        }
        else
        {
            /* ---- No sample received: check whether the link is dead ---- */
            int64_t silence = esp_timer_get_time() - last_gps_us;
            if (!link_down_shown && (last_gps_us == 0 || silence > GPS_LINK_TIMEOUT_US))
            {
                ESP_LOGE(TAG, "No NMEA data (link down?)");
                set_street_name("Sin señal GPS");
                set_var_current_speed_value(0);
                link_down_shown = true;
            }
        }
    }
}
