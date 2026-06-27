#include "sd_manager.h"
#include "tile_reader.h"
#include "waveshare_amoled_lcd_port.h"
#include "dynamic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nmea_parser.h"
#include "buttons.h"
#include "diag_log.h"
#include "net_link.h"
#include "wifi_portal.h"
#include "map_sync.h"
#include "splash.h"
#include "lvgl.h"
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

// --- Anticipation (Stage 1): heading-projected lookahead -----------------
// Below this speed the NMEA course-over-ground (cog) is noise, so we don't
// trust it for projection and freeze the last good prediction.
#define LOOKAHEAD_MIN_SPEED_KMH   15.0f
// Probe distance ahead = speed * this horizon, clamped to [MIN,MAX] meters.
// ~4 s of travel: far enough to be useful, near enough to stay on the road.
#define LOOKAHEAD_HORIZON_S       4.0f
#define LOOKAHEAD_MIN_DIST_M      60.0f
#define LOOKAHEAD_MAX_DIST_M      250.0f

// Capa A (net_link): cuanto esperar la asociacion WiFi al boot antes de seguir
// offline. Acotado para no demorar la puesta en marcha del equipo.
#define WIFI_CONNECT_TIMEOUT_MS   12000  // 12 s

// Provisioning (wifi_portal): cuanto dejar abierto el portal cautivo esperando
// que el cliente configure su red, antes de volver al modo normal.
#define WIFI_PORTAL_TIMEOUT_MS    180000 // 3 min

// Sync de tiles en segundo plano (Capa B): timeout para levantar WiFi on-demand.
#define MAP_SYNC_CONNECT_TIMEOUT_MS  20000 // 20 s

static const char *TAG = "MAIN";
static QueueHandle_t gps_queue;

// Last time any valid NMEA statement was received (link-alive heartbeat).
// Written from the event task, read from the main loop; coarse timeout only.
static volatile int64_t last_gps_us = 0;

static void calculation_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // initial delay

    // Heartbeat to the diagnostic log: the last 'hb' line before an unexpected
    // power-off marks the instant the device died; the reset reason on the next
    // boot tells us why. ~5 s cadence = every 50 iterations of this 100 ms loop.
    int hb_count = 0;

    while (1)
    {
        calculate_threshold();
        lookahead_warning_tick();
        street_message_tick();

        if (++hb_count >= 50)
        {
            hb_count = 0;
            diag_log_line("hb up=%llus heap=%u speed=%d limit=%d",
                          esp_timer_get_time() / 1000000ULL,
                          (unsigned)esp_get_free_heap_size(),
                          (int)get_var_current_speed_value(),
                          (int)get_var_speed_limit_value());
        }

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

// Full-screen overlay (on the top layer, above the EEZ screen) showing a QR the
// client scans with their phone camera to JOIN the SoftAP. The QR encodes the
// standard WiFi-join payload for an open network; once joined, the captive
// portal (DNS hijack) opens the config page automatically. lv_* is not
// thread-safe, so we go through lvgl_lock() (this runs outside the LVGL task).
static lv_obj_t *provisioning_overlay_show(const char *ssid)
{
    if (!lvgl_lock(-1))
        return NULL;

    // Contenedor a pantalla COMPLETA y opaco (tamano explicito, sin padding/
    // borde/scroll) para que tape la pantalla del mapa por completo.
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "Escanea para WiFi");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    // QR de union a la red abierta: WIFI:S:<ssid>;T:nopass;;
    char payload[80];
    int plen = snprintf(payload, sizeof(payload), "WIFI:S:%s;T:nopass;;", ssid);
    lv_obj_t *qr = lv_qrcode_create(cont, 200, lv_color_black(), lv_color_white());
    lv_res_t qr_res = lv_qrcode_update(qr, payload, plen);
    lv_obj_center(qr);
    // Borde blanco (quiet zone) alrededor del QR para que sea escaneable.
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 10, 0);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text_fmt(sub, "Red: %s", ssid);
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -36);

    lvgl_unlock();

    // Diagnostico: qr_res != LV_RES_OK significa que el encode fallo (QR en
    // blanco). Tambien registramos el heap del momento.
    diag_log_line("qr res=%d len=%d heap=%u ssid=%s",
                  (int)qr_res, plen, (unsigned)esp_get_free_heap_size(), ssid);
    return cont;
}

static void provisioning_overlay_hide(lv_obj_t *cont)
{
    if (cont && lvgl_lock(-1))
    {
        lv_obj_del(cont);
        lvgl_unlock();
    }
}

// On-demand WiFi provisioning: if the BOOT button was long-pressed, open the
// captive portal so the client can configure their network from a phone. Runs
// from the main loop (owns the screen + can reboot). Blocking while the portal
// is open; the GPS link simply resumes afterwards (matching is offline anyway).
static void maybe_run_provisioning(void)
{
    if (!buttons_take_provisioning_request())
        return;

    ESP_LOGI(TAG, "Entrando a provisioning WiFi (portal cautivo)");

    char ssid[33];
    wifi_portal_get_ap_ssid(ssid, sizeof(ssid));

    // Mostrar el QR de union al AP (el cliente lo escanea -> se une -> el portal
    // cautivo abre la pagina de configuracion solo).
    lv_obj_t *overlay = provisioning_overlay_show(ssid);

    // Partir de un WiFi limpio (por si quedo algo de un intento previo).
    net_link_stop();

    unsigned heap_before = esp_get_free_heap_size();
    esp_err_t perr = wifi_portal_run(WIFI_PORTAL_TIMEOUT_MS);
    diag_log_line("portal %s heap_before=%u",
                  (perr == ESP_OK) ? "guardado" : esp_err_to_name(perr),
                  heap_before);

    // Sacamos el overlay del QR y dejamos un mensaje en la pantalla principal,
    // con un frame para que renderice antes de reiniciar.
    provisioning_overlay_hide(overlay);
    set_street_name((perr == ESP_OK) ? "WiFi guardado. Reiniciando..."
                                      : "Reiniciando...");
    vTaskDelay(pdMS_TO_TICKS(900));

    // Parquear LVGL antes de reiniciar: tomamos lvgl_lock y NO lo soltamos, asi
    // la tarea LVGL queda bloqueada (sin hacer flush desde PSRAM) mientras el
    // apagado de WiFi dentro de esp_restart() escribe flash (deshabilita la
    // cache). Evita el 'spi transmit color failed' + watchdog del teardown.
    // Siempre reiniciamos (guardado o timeout): el boot re-lee NVS y conecta.
    lvgl_lock(-1);
    esp_restart();
}

// Sincronizacion de tiles en segundo plano (Capa B, Incr. 6). WiFi on-demand:
// levanta, baja los deltas de las zonas elegidas, apaga. Corre en su propia task
// de baja prioridad (no bloquea el loop GPS/UI) y NUNCA toca LVGL: solo deja el
// flag NVS "mapas actualizados" que el loop principal muestra como banner.
static void map_sync_task(void *arg)
{
    if (net_link_connect(MAP_SYNC_CONNECT_TIMEOUT_MS) == ESP_OK) {
        map_sync_result_t r;
        map_sync_run(&r);
        diag_log_line("map_sync: %d bajados, %d fallidos", r.downloaded, r.failed);
    } else {
        ESP_LOGW(TAG, "map_sync: WiFi no conecto, se reintenta en el proximo boot");
    }
    // net_link_stop() hace esp_wifi_deinit() (escribe flash -> cache disable). Los
    // buffers LVGL viven en RAM interna (Capa A), asi que el flush concurrente es
    // seguro. Si apareciera 'spi transmit color failed', parquear LVGL aca.
    net_link_stop();
    vTaskDelete(NULL);
}

// Dispara la sync de fondo solo si hay con que: credenciales + al menos una zona
// elegida. Asi el boot no toca WiFi cuando no esta configurado (arranque rapido).
static void maybe_start_map_sync(void)
{
    char ids[1][24];
    if (net_link_have_credentials() && map_sync_get_selected(ids, 1) > 0)
        xTaskCreatePinnedToCore(map_sync_task, "map_sync", 8192, NULL, 2, NULL, 0);
}

void app_main(void)
{
    esp_err_t err;

    waveshare_led_init();
    // El splash (logo mykeego + anillo) ya quedo cargado dentro de waveshare_led_init().
    splash_set_progress(15);

    buttons_init();
    splash_set_progress(30);

    err = sd_card_init();
    if (err != ESP_OK)
        return;
    splash_set_progress(55);

    // SD is mounted: start the diagnostic log (records the reset reason and any
    // stored crash backtrace from the previous boot).
    diag_log_init();
    splash_set_progress(70);

    // ---- Capa A: credenciales WiFi (sin conectar en el boot) ----
    // El WiFi NO se levanta en el arranque: no debe bloquear el boot ni quedar
    // encendido todo el dia. Solo importamos la pre-carga de fabrica/flota a NVS
    // si corresponde (esto no toca WiFi, solo lee/escribe NVS). La conexion se
    // hace a demanda: provisioning con long-press de BOOT (portal cautivo), y mas
    // adelante la descarga de tiles de la Capa B (connect -> bajar -> stop). El
    // equipo arranca rapido y opera offline con los tiles que ya tiene.
    net_link_import_sd_config();
    diag_log_line(net_link_have_credentials()
                      ? "wifi cfg ok (on-demand, no se conecta en boot)"
                      : "wifi sin credenciales (manten BOOT para configurar)");
    splash_set_progress(80);

    // Selftest del Incremento 2 (Capa B): solo si CONFIG_MAP_SYNC_BOOT_SELFTEST.
    // Conecta WiFi STA, baja catalog.json, loguea las zonas y desconecta. No-op en prod.
    map_sync_selftest();

    // 6144 (was 4096): this task now also writes the heartbeat to the SD card,
    // and FATFS/fopen paths are stack-hungry.
    xTaskCreate(calculation_task, "calculation_task", 6144, NULL, 5, NULL);
    gps_queue = xQueueCreate(5, sizeof(gps_t));

    /* NMEA parser configuration */
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    config.uart.baud_rate = 115200;
    /* init NMEA parser library */
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&config);
    /* register event handler for NMEA parser library */
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);
    splash_set_progress(90);

    // Inicializacion completa: fundido del splash hacia la pantalla principal.
    splash_finish();

    // Capa B: si hay credenciales + zonas elegidas, lanza la sync de tiles en
    // segundo plano (WiFi on-demand, no bloquea el arranque ni el loop).
    maybe_start_map_sync();

    bool link_down_shown = false;

    while (1)
    {
        // Handle a pending WiFi-provisioning request (BOOT long-press) before
        // processing GPS. May reboot the device if the client saves a network.
        maybe_run_provisioning();

        // Banner "mapas actualizados": lo deja la sync de fondo (este boot o el
        // anterior) via flag NVS; se muestra una vez y se limpia.
        if (map_sync_updates_pending()) {
            show_temp_message("Mapas actualizados", 4000);
            map_sync_clear_updates_flag();
        }

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

            // Anchor the diagnostic log to GPS wall-clock time (no RTC at boot).
            diag_log_set_walltime(gps.date.year + YEAR_BASE, gps.date.month, gps.date.day,
                                  gps.tim.hour + TIME_ZONE, gps.tim.minute, gps.tim.second);

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

            /* ---- 4. Anticipation: heading-projected lookahead ---- */
            // cog is unreliable at low speed, so gate on a minimum speed and
            // freeze the last good prediction below it. The horizon scales with
            // speed (clamped) so the probe stays on the road we're approaching.
            if (kmh >= LOOKAHEAD_MIN_SPEED_KMH && isfinite(gps.cog))
            {
                float horizon = (kmh / 3.6f) * LOOKAHEAD_HORIZON_S; // m/s * s = m
                if (horizon < LOOKAHEAD_MIN_DIST_M) horizon = LOOKAHEAD_MIN_DIST_M;
                if (horizon > LOOKAHEAD_MAX_DIST_M) horizon = LOOKAHEAD_MAX_DIST_M;

                lookahead_t la = { .valid = true, .distance_m = (int)horizon };
                float plat = 0, plon = 0;
                if (get_speed_and_name_at_lookahead(gps.latitude, gps.longitude, gps.cog,
                                                    horizon, &la.speed_limit,
                                                    la.street, sizeof(la.street),
                                                    &plat, &plon))
                {
                    set_lookahead(&la);
                    // Stage 1a: validate the dead-reckoning math on a real drive
                    // (no UI yet). Confirm the probe lands on the road ahead and
                    // "next" street is the upcoming one, not the parallel/opposite.
                    diag_log_line("look cog=%.0f d=%dm @%.05f,%.05f -> %s lim=%d",
                                  gps.cog, (int)horizon, plat, plon,
                                  la.street, la.speed_limit);
                }
                else
                {
                    ESP_LOGD(TAG, "Lookahead: no street %dm ahead (cog=%.0f)",
                             (int)horizon, gps.cog);
                }
            }
            else
            {
                clear_lookahead(); // too slow / no heading: don't trust cog
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
