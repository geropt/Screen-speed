#include "sd_manager.h"
#include "tile_reader.h"
#include "tile_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "waveshare_amoled_lcd_port.h"
#include "dynamic.h"
#include "splash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <inttypes.h>
#include <string.h>
#include "nmea_parser.h"
#include "vehicle_state.h"
#include "res_metrics.h"

#define YEAR_BASE (2000) // date in GPS starts from 2000

static const char *TAG = "MAIN";

/* Mailbox de fixes: un solo lugar, con sobrescritura.
 *
 * Antes era una cola de 5 elementos de `gps_t` completo. Con un consumidor lento
 * —una tarjeta SD trabada, un barrido de 9 tiles— esa cola acumulaba fixes
 * vencidos y después los procesaba en ráfaga como si fueran actuales, gastando
 * lecturas de SD en posiciones por las que el vehículo ya había pasado. Un
 * mailbox de un lugar siempre entrega el último fix conocido y descarta los
 * intermedios, que es lo que corresponde para un dato de estado.
 *
 * El outbox de telemetría es otra cosa y va aparte: ahí ningún dato se puede
 * perder por sobrescritura. Se define en P05. */
static QueueHandle_t s_fix_mailbox;

/* Cola de señales de IO e identidad. Estas no se pueden sobrescribir: un cambio
 * de ignición o de IMEI perdido cambia el significado del resto del estado. */
#define SIGNAL_QUEUE_DEPTH 8
static QueueHandle_t s_signal_queue;

/* Estado del vehículo. Dueño único: la tarea principal. Nadie más lo toca. */
static vehicle_state_t s_vehicle;

/* Disponibilidad de la tarjeta. La escribe la tarea que vigila el montaje y la
 * lee la tarea principal para no intentar 9 aperturas de tile por fix cuando no
 * hay tarjeta. `volatile` alcanza: es un booleano de un solo escritor. */
static volatile bool s_sd_mounted;

static uint64_t mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---------- Productores: corren en la tarea que drena la UART ----------
 *
 * Regla de P02b: ningún callback del enlace toca UI, SD, DNS ni espera ACK. Todos
 * arman un mensaje acotado y vuelven. Si la cola está llena se cuenta y se
 * descarta, en lugar de bloquear el drenaje de la UART.
 */

static void on_gps_update(void *event_handler_arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)event_base;
    if (event_id != GPS_UPDATE || !event_data) {
        return;
    }
    const gps_t *gps = (const gps_t *)event_data;

    /* El baud al que enganchó el enlace, una sola vez. */
    static bool baud_logged = false;
    if (!baud_logged) {
        ESP_LOGI(TAG, "enlace NMEA a %" PRIu32 " baud",
                 nmea_parser_get_baud(event_handler_arg));
        baud_logged = true;
    }

    /* El log completo por fix quedaba en nivel INFO y con 11 argumentos: a 1 Hz
     * es tráfico constante por la consola desde la tarea que drena la UART. Pasa
     * a DEBUG. */
    ESP_LOGD(TAG, "%d/%d/%d %02d:%02d:%02d lat=%.5f lon=%.5f alt=%.1f v=%.1f m/s",
             gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
             gps->tim.hour, gps->tim.minute, gps->tim.second,
             gps->latitude, gps->longitude, gps->altitude, gps->speed);

    /* Mensaje acotado: 40 B en lugar del `gps_t` completo. */
    vehicle_msg_t msg = {0};
    msg.kind = VEHICLE_MSG_FIX;
    msg.mono_ms = mono_ms();
    msg.u.fix.lat = gps->latitude;
    msg.u.fix.lon = gps->longitude;
    msg.u.fix.speed_kmh = gps->speed * 3.6f;
    msg.u.fix.cog_deg = gps->cog;

    /* xQueueOverwrite, no xQueueSendFromISR. Esto corre en la tarea del parser,
     * no en una ISR: usar la variante FromISR desde una tarea era un error de API
     * que además dejaba el contexto de cambio de tarea sin atender. */
    xQueueOverwrite(s_fix_mailbox, &msg);
}

static void post_signal(const vehicle_msg_t *msg)
{
    if (xQueueSend(s_signal_queue, msg, 0) != pdTRUE) {
        /* Cola llena: se cuenta. No se bloquea la tarea de UART esperando lugar. */
        res_metrics_error(RES_ERR_QUEUE_FULL);
    }
}

static void on_ignition(void *ctx, bool ignition_on)
{
    (void)ctx;
    vehicle_msg_t msg = {0};
    msg.kind = VEHICLE_MSG_IGNITION;
    msg.mono_ms = mono_ms();
    msg.u.ignition.on = ignition_on;
    post_signal(&msg);
}

static void on_gprs(void *ctx, bool gprs_up)
{
    (void)ctx;
    vehicle_msg_t msg = {0};
    msg.kind = VEHICLE_MSG_GPRS;
    msg.mono_ms = mono_ms();
    msg.u.gprs.up = gprs_up;
    post_signal(&msg);
}

static void on_imei(void *ctx, const char *imei)
{
    (void)ctx;
    vehicle_msg_t msg = {0};
    msg.kind = VEHICLE_MSG_IMEI;
    msg.mono_ms = mono_ms();
    strncpy(msg.u.imei.digits, imei, VEHICLE_IMEI_MAX_LEN);
    post_signal(&msg);
}

/* ---------- Tarjeta SD: arranque degradado y recuperación ---------- */

/**
 * @brief Vigila la tarjeta sin reiniciar nada más.
 *
 * Antes `app_main` hacía `return` si el montaje fallaba: sin tarjeta no había
 * UART, ni estado, ni velocidad en pantalla. El velocímetro no depende de los
 * mapas, así que ahora el arranque sigue sin tarjeta y esta tarea reintenta.
 *
 * Al montar, sólo se marca la disponibilidad: no hay reset global ni se reinicia
 * el enlace. La generación de mapas y la barrera de lectores son de P04/P07.
 */
static void sd_watch_task(void *arg)
{
    (void)arg;
    res_metrics_watch_task("sd_watch", NULL);

    const TickType_t retry_period = pdMS_TO_TICKS(5000);
    bool announced_missing = false;

    while (1) {
        if (!s_sd_mounted) {
            esp_err_t err = sd_card_init();
            if (err == ESP_OK) {
                s_sd_mounted = true;
                announced_missing = false;
                ESP_LOGI(TAG, "tarjeta montada; mapas disponibles");
            } else {
                if (!announced_missing) {
                    ESP_LOGW(TAG, "sin tarjeta (%s): se sigue mostrando velocidad, "
                                  "sin límite de mapa. Reintento cada 5 s",
                             esp_err_to_name(err));
                    res_metrics_error(RES_ERR_SD_IO);
                    announced_missing = true;
                }
            }
        }
        vTaskDelay(retry_period);
    }
}

/* ---------- Consumidor: dueño del estado ---------- */

static void apply_pending_signals(void)
{
    vehicle_msg_t msg;
    /* Drenar la cola de señales antes de mirar el fix, para que el snapshot
     * incluya todo lo recibido hasta ahora. */
    while (xQueueReceive(s_signal_queue, &msg, 0) == pdTRUE) {
        uint64_t epoch_before = s_vehicle.tracker_epoch;
        vehicle_state_apply(&s_vehicle, &msg);
        if (s_vehicle.tracker_epoch != epoch_before) {
            ESP_LOGW(TAG, "cambió el IMEI: época %" PRIu64 ", estado anterior invalidado",
                     s_vehicle.tracker_epoch);
        }
    }
}

static void log_state_periodically(const vehicle_snapshot_t *snap)
{
    static uint32_t n;
    if ((++n % 60) != 0) {
        return;
    }
    ESP_LOGI(TAG, "estado: fix=%s(%" PRIu32 " ms) ignición=%s gprs=%s imei=%s época=%" PRIu64,
             vs_state_name(snap->fix_state), snap->fix_age_ms,
             vs_state_name(snap->ignition_state),
             vs_state_name(snap->gprs_state),
             snap->imei_known ? snap->imei : "desconocido",
             snap->tracker_epoch);

    if (s_sd_mounted) {
        uint32_t hits, misses, entries;
        size_t bytes;
        tile_cache_stats(&hits, &misses, &bytes, &entries);
        uint32_t total = hits + misses;
        ESP_LOGI(TAG, "tiles: %" PRIu32 "%% hits (%" PRIu32 "/%" PRIu32
                      "), %u KB en %" PRIu32 " entradas, PSRAM libre %u KB",
                 total ? (hits * 100 / total) : 0, hits, total,
                 (unsigned)(bytes / 1024), entries,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

static void calculation_task(void *pvParameter)
{
    (void)pvParameter;
    vTaskDelay(pdMS_TO_TICKS(2000)); // initial delay

    while (1)
    {
        calculate_threshold();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    res_metrics_init();
    vehicle_state_config_t vs_cfg = VEHICLE_STATE_CONFIG_DEFAULT();
    vehicle_state_init(&s_vehicle, &vs_cfg);

    waveshare_led_init();
    // El splash (logo mykeego + anillo) ya quedo cargado dentro de waveshare_led_init().

    s_fix_mailbox = xQueueCreate(1, sizeof(vehicle_msg_t));
    s_signal_queue = xQueueCreate(SIGNAL_QUEUE_DEPTH, sizeof(vehicle_msg_t));
    if (!s_fix_mailbox || !s_signal_queue) {
        /* Sin transporte de mensajes no hay nada que hacer, y seguir en silencio
         * dejaría una pantalla encendida que nunca actualiza. */
        ESP_LOGE(TAG, "no se pudieron crear las colas de estado");
        res_metrics_error(RES_ERR_ALLOC_FAILED);
        return;
    }

    /* La tarjeta ya NO condiciona el arranque. El velocímetro no depende de los
     * mapas: con la tarjeta ausente se muestra velocidad y no se muestra límite,
     * y la tarea de vigilancia la monta cuando aparezca. */
    if (xTaskCreate(sd_watch_task, "sd_watch", 4096, NULL, 4, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "no se pudo crear sd_watch");
        res_metrics_error(RES_ERR_ALLOC_FAILED);
    }
    splash_set_progress(40);

    if (xTaskCreate(calculation_task, "calculation_task", 4096, NULL, 5, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "no se pudo crear calculation_task");
        res_metrics_error(RES_ERR_ALLOC_FAILED);
    }

    /* NMEA parser configuration. Initial baud rate comes from
     * CONFIG_NMEA_PARSER_UART_BAUD_RATE; the parser auto-probes between
     * 9600 and 115200 until it sees NMEA with a valid checksum, so it
     * works with both Pro5-Lite/HCV5-Lite (capped at 9600) and Pro5/HCV5
     * (115200) trackers without reflashing. */
    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&config);
    if (!nmea_hdl) {
        /* Antes este retorno no se miraba. Sin parser no hay velocidad, así que
         * tiene que quedar dicho en el log en lugar de fallar callado más tarde. */
        ESP_LOGE(TAG, "no se pudo inicializar el parser NMEA");
        res_metrics_error(RES_ERR_ALLOC_FAILED);
        return;
    }
    esp_err_t err = nmea_parser_add_handler(nmea_hdl, on_gps_update, nmea_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo registrar el handler de GPS: %s", esp_err_to_name(err));
    }
    err = nmea_parser_set_signal_handlers(nmea_hdl, on_ignition, on_gprs, on_imei, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudieron registrar las señales de IO: %s", esp_err_to_name(err));
    }
    splash_set_progress(90);

    // Inicializacion completa: fundido del splash hacia la pantalla principal.
    splash_finish();

    res_metrics_watch_task("main", NULL);

    while (1)
    {
        vehicle_msg_t fix_msg;
        /* Espera acotada en lugar de portMAX_DELAY: el estado envejece aunque no
         * llegue ningún fix, y hay que poder actualizar la pantalla cuando el dato
         * vence. Sin esto, «vencido» no se podría mostrar hasta el próximo fix. */
        bool got_fix = (xQueueReceive(s_fix_mailbox, &fix_msg, pdMS_TO_TICKS(250)) == pdTRUE);

        apply_pending_signals();
        if (got_fix) {
            vehicle_state_apply(&s_vehicle, &fix_msg);
        }

        vehicle_snapshot_t snap;
        vehicle_state_snapshot(&s_vehicle, mono_ms(), &snap);

        /* La velocidad se muestra con el estado válido, sin esperar el matching.
         * Presentar «vencido» y «desconocido» de forma distinta en pantalla es de
         * P03; acá el dato ya viaja con su edad. */
        if (snap.fix_state == VS_FRESH) {
            set_var_current_speed_value((int32_t)snap.speed_kmh);
        }

        /* El matching se intenta sólo con un fix fresco de la época actual y con
         * tarjeta montada. Con la tarjeta ausente se evita gastar 9 aperturas
         * fallidas por fix. */
        if (got_fix && snap.fix_state == VS_FRESH && snap.fix_epoch_current && s_sd_mounted) {
            int speed_limit = 0;
            char street[128];
            street[0] = '\0';
            res_metrics_begin(RES_CH_MAP_MATCH);
            bool matched = get_speed_and_name_at((float)snap.lat, (float)snap.lon,
                                                 snap.cog_deg, snap.speed_kmh,
                                                 &speed_limit, street, sizeof(street));
            res_metrics_end(RES_CH_MAP_MATCH);

            if (matched) {
                /* El resultado se calculó con esta época; si el IMEI cambió
                 * mientras se leían tiles, hay que descartarlo en lugar de
                 * mostrar el límite de la calle del vehículo anterior. */
                if (s_vehicle.tracker_epoch == snap.tracker_epoch) {
                    ESP_LOGD(TAG, "límite %d km/h en %s", speed_limit, street);
                    set_street_name(street);
                    set_var_speed_limit_value(speed_limit);
                } else {
                    ESP_LOGW(TAG, "resultado de mapa descartado: cambió la época");
                }
            } else {
                ESP_LOGD(TAG, "sin datos de mapa para esta posición");
                // No se borra el ultimo limite conocido, por pedido del cliente.
            }
        }

        log_state_periodically(&snap);
    }
}
