#include "vehicle_state.h"

#include <string.h>

/* Edad acotada a 32 bits. Un dato de más de 49 días es tan vencido como uno de
 * 49 días: no hace falta más rango y así el snapshot queda chico. */
static uint32_t age_of(uint64_t now_ms, uint64_t then_ms)
{
    if (now_ms <= then_ms) {
        return 0;
    }
    uint64_t d = now_ms - then_ms;
    return (d > UINT32_MAX) ? UINT32_MAX : (uint32_t)d;
}

static vs_state_t freshness(bool seen, uint64_t now_ms, uint64_t then_ms,
                            uint32_t ttl_ms, uint32_t *out_age)
{
    if (!seen) {
        *out_age = 0;
        return VS_UNKNOWN;
    }
    uint32_t age = age_of(now_ms, then_ms);
    *out_age = age;
    return (age <= ttl_ms) ? VS_FRESH : VS_EXPIRED;
}

void vehicle_state_init(vehicle_state_t *vs, const vehicle_state_config_t *cfg)
{
    if (!vs) {
        return;
    }
    memset(vs, 0, sizeof(*vs));
    if (cfg) {
        vs->cfg = *cfg;
    } else {
        vehicle_state_config_t d = VEHICLE_STATE_CONFIG_DEFAULT();
        vs->cfg = d;
    }
    /* La época arranca en 1 para que 0 signifique «ninguna época», y un resultado
     * calculado antes de tener estado se pueda distinguir. */
    vs->tracker_epoch = 1;
}

void vehicle_state_new_epoch(vehicle_state_t *vs)
{
    if (!vs) {
        return;
    }
    vs->tracker_epoch++;

    /* Todo lo recibido pertenecía al tracker anterior. Se descarta en lugar de
     * mezclarse: una posición del vehículo anterior es peor que no tener
     * posición. Los contadores se conservan porque describen la vida del
     * proceso, no la de la época. */
    vs->fix_seen = false;
    vs->fix_mono_ms = 0;
    vs->fix_lat = 0.0;
    vs->fix_lon = 0.0;
    vs->fix_speed_kmh = 0.0f;
    vs->fix_cog_deg = 0.0f;
    vs->fix_epoch = 0;

    vs->ignition_seen = false;
    vs->ignition_mono_ms = 0;
    vs->ignition_on = false;

    vs->gprs_seen = false;
    vs->gprs_mono_ms = 0;
    vs->gprs_up = false;
}

bool vehicle_state_apply(vehicle_state_t *vs, const vehicle_msg_t *msg)
{
    if (!vs || !msg) {
        return false;
    }

    switch (msg->kind) {
    case VEHICLE_MSG_FIX:
        if (vs->fix_seen && msg->mono_ms < vs->fix_mono_ms) {
            vs->rejected_stale_clock++;
            return false;
        }
        vs->fix_seen = true;
        vs->fix_mono_ms = msg->mono_ms;
        vs->fix_lat = msg->u.fix.lat;
        vs->fix_lon = msg->u.fix.lon;
        vs->fix_speed_kmh = msg->u.fix.speed_kmh;
        vs->fix_cog_deg = msg->u.fix.cog_deg;
        vs->fix_epoch = vs->tracker_epoch;
        vs->fixes++;
        return true;

    case VEHICLE_MSG_IGNITION:
        if (vs->ignition_seen && msg->mono_ms < vs->ignition_mono_ms) {
            vs->rejected_stale_clock++;
            return false;
        }
        vs->ignition_seen = true;
        vs->ignition_mono_ms = msg->mono_ms;
        vs->ignition_on = msg->u.ignition.on;
        vs->ignition_updates++;
        return true;

    case VEHICLE_MSG_GPRS:
        if (vs->gprs_seen && msg->mono_ms < vs->gprs_mono_ms) {
            vs->rejected_stale_clock++;
            return false;
        }
        vs->gprs_seen = true;
        vs->gprs_mono_ms = msg->mono_ms;
        vs->gprs_up = msg->u.gprs.up;
        vs->gprs_updates++;
        return true;

    case VEHICLE_MSG_IMEI: {
        const char *incoming = msg->u.imei.digits;
        if (incoming[0] == '\0') {
            return false;
        }
        if (vs->imei_known && strncmp(vs->imei, incoming, VEHICLE_IMEI_MAX_LEN) == 0) {
            /* Mismo tracker: repetir el marcador no cambia nada y sobre todo NO
             * rejuvenece ninguna otra señal. Se reinicia el conteo de lecturas
             * discrepantes, porque la corrida de discrepancias se cortó. */
            vs->imei_pending_count = 0;
            vs->imei_pending[0] = '\0';
            return false;
        }

        if (!vs->imei_known) {
            /* Primera vez que se conoce el IMEI: no es un cambio de tracker, así que
             * no se tira el fix que ya se tenía. Se acepta de una. */
            strncpy(vs->imei, incoming, VEHICLE_IMEI_MAX_LEN);
            vs->imei[VEHICLE_IMEI_MAX_LEN] = '\0';
            vs->imei_known = true;
            vs->imei_pending_count = 0;
            vs->imei_pending[0] = '\0';
            return true;
        }

        /* Discrepa del IMEI conocido. NO se declara cambio de tracker con una sola
         * lectura.
         *
         * Motivo, medido sobre una captura de campo: el marcador `###IMEI` viaja sin
         * checksum, y en 1 809 lecturas apareció una corrupta —un dígito de más—.
         * Aceptarla habría subido `tracker_epoch` y descartado posición, ignición y
         * GPRS por un byte perdido en la serie. Como el marcador se repite varias
         * veces por segundo, exigir lecturas consecutivas coincidentes no retrasa un
         * cambio real y descarta el ruido. */
        if (strncmp(vs->imei_pending, incoming, VEHICLE_IMEI_MAX_LEN) == 0) {
            vs->imei_pending_count++;
        } else {
            strncpy(vs->imei_pending, incoming, VEHICLE_IMEI_MAX_LEN);
            vs->imei_pending[VEHICLE_IMEI_MAX_LEN] = '\0';
            vs->imei_pending_count = 1;
        }

        if (vs->imei_pending_count < VEHICLE_IMEI_CONFIRMATIONS) {
            vs->imei_rejected_single++;
            return false;
        }

        /* Confirmado: cambió de tracker. Época nueva y estado invalidado. */
        strncpy(vs->imei, incoming, VEHICLE_IMEI_MAX_LEN);
        vs->imei[VEHICLE_IMEI_MAX_LEN] = '\0';
        vs->imei_pending_count = 0;
        vs->imei_pending[0] = '\0';
        vs->imei_changes++;
        vehicle_state_new_epoch(vs);
        return true;
    }

    default:
        return false;
    }
}

void vehicle_state_snapshot(const vehicle_state_t *vs, uint64_t now_ms,
                           vehicle_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!vs) {
        return;
    }

    out->mono_ms = now_ms;
    out->tracker_epoch = vs->tracker_epoch;

    out->fix_state = freshness(vs->fix_seen, now_ms, vs->fix_mono_ms,
                               vs->cfg.fix_ttl_ms, &out->fix_age_ms);
    out->lat = vs->fix_lat;
    out->lon = vs->fix_lon;
    out->speed_kmh = vs->fix_speed_kmh;
    out->cog_deg = vs->fix_cog_deg;
    out->fix_epoch_current = (vs->fix_seen && vs->fix_epoch == vs->tracker_epoch);

    out->ignition_state = freshness(vs->ignition_seen, now_ms, vs->ignition_mono_ms,
                                    vs->cfg.ignition_ttl_ms, &out->ignition_age_ms);
    out->ignition_on = vs->ignition_on;

    out->gprs_state = freshness(vs->gprs_seen, now_ms, vs->gprs_mono_ms,
                                vs->cfg.gprs_ttl_ms, &out->gprs_age_ms);
    out->gprs_up = vs->gprs_up;

    out->imei_known = vs->imei_known;
    memcpy(out->imei, vs->imei, sizeof(out->imei));
}

const char *vs_state_name(vs_state_t s)
{
    switch (s) {
    case VS_UNKNOWN: return "desconocido";
    case VS_FRESH:   return "fresco";
    case VS_EXPIRED: return "vencido";
    default:         return "?";
    }
}
