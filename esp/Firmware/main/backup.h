/* Respaldo de telemetría en el HUD: Wi-Fi + cmd 68 con ACK.
 *
 * Porta lo que el piloto `starlink_pilot` ya demostró en banco, usando los
 * contratos de P05 (política, outbox, dueño de la radio). La tarea de respaldo
 * nunca toca LVGL ni la SD: encola records desde UART y habla con el servidor
 * en su propio hilo.
 */
#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "nmea_parser.h"
#include "vehicle_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BACKUP_ENABLE

esp_err_t backup_start(nmea_parser_handle_t nmea_hdl);

/** Copia acotada del snapshot. La llama la tarea principal; no bloquea. */
void backup_publish_snapshot(const vehicle_snapshot_t *snap);

#else

static inline esp_err_t backup_start(nmea_parser_handle_t nmea_hdl)
{
    (void)nmea_hdl;
    return ESP_OK;
}

static inline void backup_publish_snapshot(const vehicle_snapshot_t *snap)
{
    (void)snap;
}

#endif

#ifdef __cplusplus
}
#endif
