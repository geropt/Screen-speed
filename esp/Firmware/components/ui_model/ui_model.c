#include "ui_model.h"

#include <string.h>

static uint32_t age_of(uint64_t now_ms, uint64_t then_ms)
{
    if (now_ms <= then_ms) {
        return 0;
    }
    uint64_t d = now_ms - then_ms;
    return (d > UINT32_MAX) ? UINT32_MAX : (uint32_t)d;
}

void ui_model_init(ui_model_state_t *st, const ui_model_config_t *cfg)
{
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
    if (cfg) {
        st->cfg = *cfg;
    } else {
        ui_model_config_t d = UI_MODEL_CONFIG_DEFAULT();
        st->cfg = d;
    }
}

void ui_model_invalidate(ui_model_state_t *st)
{
    if (!st) {
        return;
    }
    st->limit_seen = false;
    st->limit_kmh = 0;
    st->limit_anonymous = false;
    st->limit_mono_ms = 0;
    st->limit_epoch = 0;
    st->street[0] = '\0';
    st->street_known = false;
    st->overspeed_latched = false;
}

void ui_model_on_match(ui_model_state_t *st, uint64_t now_ms,
                      const ui_match_input_t *in, uint64_t current_epoch)
{
    if (!st || !in) {
        return;
    }
    /* Un resultado calculado con otra época pertenece a otro tracker. Mostrarlo
     * sería peor que no mostrar nada. */
    if (in->tracker_epoch != current_epoch) {
        return;
    }
    if (!in->matched) {
        ui_model_on_no_match(st, now_ms);
        return;
    }

    st->limit_mono_ms = now_ms;
    st->limit_epoch = current_epoch;
    st->limit_anonymous = in->anonymous;

    if (in->has_limit && in->limit_kmh > 0) {
        st->limit_seen = true;
        st->limit_kmh = in->limit_kmh;
    }
    /* Si el tramo no declara límite no se inventa uno: se conserva el anterior y
     * el estado resultante lo dirá. */

    if (!in->anonymous && in->street[0] != '\0') {
        strncpy(st->street, in->street, UI_STREET_MAX - 1);
        st->street[UI_STREET_MAX - 1] = '\0';
        st->street_known = true;
    }
}

void ui_model_on_no_match(ui_model_state_t *st, uint64_t now_ms)
{
    (void)now_ms;
    if (!st) {
        return;
    }
    /* No se borra el último límite conocido: el cliente pidió conservarlo. Lo que
     * NO se hace es refrescar su marca de tiempo, así que envejece y termina
     * declarándose vencido en lugar de presentarse como si fuera del tramo actual. */
}

void ui_model_build(ui_model_state_t *st, const vehicle_snapshot_t *snap,
                   bool sd_present, ui_model_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!st || !snap) {
        return;
    }

    if (snap->fix_state != VS_UNKNOWN || snap->imei_known ||
        snap->ignition_state != VS_UNKNOWN) {
        st->link_seen = true;
    }

    out->sd_present = sd_present;
    out->ignition_known = (snap->ignition_state != VS_UNKNOWN);
    out->ignition_on = out->ignition_known ? snap->ignition_on : false;

    /* --- Velocidad ---
     * Se muestra en cuanto hay un fix válido, sin esperar el matching: es el dato
     * que el producto tiene que dar siempre y no depende de la tarjeta. */
    switch (snap->fix_state) {
    case VS_FRESH:
        out->speed_state = UI_SPEED_LIVE;
        out->speed_kmh = (int32_t)snap->speed_kmh;
        break;
    case VS_EXPIRED:
        /* El número sigue disponible pero ya no es actual, y hay que decirlo. */
        out->speed_state = UI_SPEED_STALE;
        out->speed_kmh = (int32_t)snap->speed_kmh;
        break;
    case VS_UNKNOWN:
    default:
        out->speed_state = UI_SPEED_UNKNOWN;
        out->speed_kmh = 0;
        break;
    }

    /* --- Límite ---
     * Cuatro estados distintos, que es lo que permite no mentir: nunca se supo,
     * se sabe del tramo actual, se sabe pero ya no se confirma, o venció. */
    if (!st->limit_seen) {
        out->limit_state = UI_LIMIT_UNKNOWN;
        out->limit_kmh = 0;
    } else {
        uint32_t limit_age = age_of(snap->mono_ms, st->limit_mono_ms);
        bool epoch_ok = (st->limit_epoch == snap->tracker_epoch);

        if (!epoch_ok) {
            /* El límite es de otro tracker. */
            out->limit_state = UI_LIMIT_UNKNOWN;
            out->limit_kmh = 0;
        } else if (limit_age > st->cfg.limit_last_known_ttl_ms) {
            out->limit_state = UI_LIMIT_EXPIRED;
            out->limit_kmh = st->limit_kmh;
        } else if (st->limit_anonymous) {
            /* Vía sin nombre: el número existe pero no se puede atribuir a una
             * calle, así que se presenta declarado como inferencia. */
            out->limit_state = UI_LIMIT_INFERRED;
            out->limit_kmh = st->limit_kmh;
        } else if (limit_age == 0) {
            out->limit_state = UI_LIMIT_KNOWN;
            out->limit_kmh = st->limit_kmh;
        } else {
            out->limit_state = UI_LIMIT_LAST_KNOWN;
            out->limit_kmh = st->limit_kmh;
        }
    }

    /* --- Calle --- */
    if (st->street_known && st->limit_epoch == snap->tracker_epoch) {
        out->show_street = true;
        memcpy(out->street, st->street, UI_STREET_MAX);
    } else {
        out->show_street = false;
        out->street[0] = '\0';
    }

    /* --- Alerta de exceso, con histéresis ---
     * Sólo se evalúa con velocidad en vivo y un límite atribuible al tramo. Con el
     * fix vencido o el límite vencido no se alerta: alertar sobre datos que se
     * declaran no actuales sería peor que no alertar. */
    bool eligible = (out->speed_state == UI_SPEED_LIVE) &&
                    (out->limit_state == UI_LIMIT_KNOWN ||
                     out->limit_state == UI_LIMIT_LAST_KNOWN ||
                     out->limit_state == UI_LIMIT_INFERRED) &&
                    (out->limit_kmh > 0) &&
                    (out->speed_kmh >= st->cfg.alert_min_speed_kmh);

    if (!eligible) {
        st->overspeed_latched = false;
    } else if (st->overspeed_latched) {
        /* Ya encendida: se apaga sólo al bajar del umbral inferior. */
        if (out->speed_kmh <= out->limit_kmh + st->cfg.over_off_kmh) {
            st->overspeed_latched = false;
        }
    } else {
        if (out->speed_kmh > out->limit_kmh + st->cfg.over_on_kmh) {
            st->overspeed_latched = true;
        }
    }
    out->overspeed = st->overspeed_latched;

    /* --- Pantalla ---
     * El orden importa: sin enlace todavía no se sabe nada; con enlace pero sin
     * fix la falla es del GPS; sin tarjeta el velocímetro funciona igual y lo que
     * falta son los mapas. */
    if (!st->link_seen) {
        out->screen = UI_SCREEN_WAITING_LINK;
    } else if (out->speed_state == UI_SPEED_UNKNOWN) {
        out->screen = UI_SCREEN_NO_FIX;
    } else if (!sd_present) {
        out->screen = UI_SCREEN_NO_MAPS;
    } else {
        out->screen = UI_SCREEN_DRIVING;
    }
}

const char *ui_limit_state_name(ui_limit_state_t s)
{
    switch (s) {
    case UI_LIMIT_UNKNOWN:    return "desconocido";
    case UI_LIMIT_KNOWN:      return "conocido";
    case UI_LIMIT_LAST_KNOWN: return "ultimo_conocido";
    case UI_LIMIT_INFERRED:   return "inferido";
    case UI_LIMIT_EXPIRED:    return "vencido";
    default:                  return "?";
    }
}

const char *ui_speed_state_name(ui_speed_state_t s)
{
    switch (s) {
    case UI_SPEED_UNKNOWN: return "desconocida";
    case UI_SPEED_LIVE:    return "en_vivo";
    case UI_SPEED_STALE:   return "vencida";
    default:               return "?";
    }
}

const char *ui_screen_name(ui_screen_t s)
{
    switch (s) {
    case UI_SCREEN_WAITING_LINK: return "esperando_enlace";
    case UI_SCREEN_DRIVING:      return "conduciendo";
    case UI_SCREEN_NO_FIX:       return "sin_fix";
    case UI_SCREEN_NO_MAPS:      return "sin_mapas";
    default:                     return "?";
    }
}
