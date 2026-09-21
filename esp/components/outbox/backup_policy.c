#include "backup_policy.h"

#include <string.h>

void backup_policy_init(backup_policy_t *p, const backup_policy_config_t *cfg)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    if (cfg) {
        p->cfg = *cfg;
    } else {
        backup_policy_config_t d = BACKUP_POLICY_CONFIG_DEFAULT();
        p->cfg = d;
    }
}

/* Deja una revocación pendiente si había permiso. Idempotente. */
static void revoke(backup_policy_t *p, uint64_t now_ms)
{
    if (!p->granted) {
        return;
    }
    p->granted = false;
    p->revoke_pending = true;
    p->revoked_at_ms = now_ms;
    p->revocations++;
}

backup_verdict_t backup_policy_evaluate(backup_policy_t *p, uint64_t now_ms,
                                       const backup_inputs_t *in)
{
    if (!p || !in) {
        return BACKUP_DENY_NO_IMEI;
    }

    /* --- Identidad ---
     * Va primero: sin IMEI no se puede armar un paquete, y con otro IMEI el permiso
     * no se transfiere. Un tracker distinto es otro vehículo o otro equipo. */
    if (!in->imei_known || in->imei[0] == '\0') {
        revoke(p, now_ms);
        p->down_since_ms = 0;
        return BACKUP_DENY_NO_IMEI;
    }
    if (p->granted_imei[0] != '\0' &&
        strncmp(p->granted_imei, in->imei, BACKUP_IMEI_MAX) != 0) {
        revoke(p, now_ms);
        p->down_since_ms = 0;
        /* La identidad nueva se adopta para que un permiso posterior pueda
         * concederse a ella, pero este ciclo se deniega. */
        strncpy(p->granted_imei, in->imei, BACKUP_IMEI_MAX);
        p->granted_imei[BACKUP_IMEI_MAX] = '\0';
        return BACKUP_DENY_IMEI_CHANGED;
    }

    /* --- Estado de GPRS del tracker --- */
    if (!in->gprs_known) {
        /* Nunca se supo. La ausencia de información no es permiso: si el tracker
         * está enviando y el HUD también, se duplican los datos. */
        revoke(p, now_ms);
        p->down_since_ms = 0;
        return BACKUP_DENY_GPRS_UNKNOWN;
    }
    if (in->gprs_age_ms > p->cfg.gprs_fresh_ms) {
        /* Un 418 viejo no dice nada del presente. */
        revoke(p, now_ms);
        p->down_since_ms = 0;
        return BACKUP_DENY_GPRS_STALE;
    }
    if (in->gprs_up) {
        /* El tracker volvió: revocación inmediata y se reinicia el dwell. */
        revoke(p, now_ms);
        p->down_since_ms = 0;
        return BACKUP_DENY_GPRS_UP;
    }

    /* --- Dwell: GPRS caído de forma continua --- */
    if (p->down_since_ms == 0) {
        p->down_since_ms = now_ms;
    }
    uint64_t down_for = (now_ms > p->down_since_ms) ? (now_ms - p->down_since_ms) : 0;
    if (down_for < p->cfg.dwell_ms) {
        /* Caído pero hace poco: no se toma el relevo todavía. No se revoca porque
         * no había permiso; si lo había, el GPRS estaba arriba y ya se revocó. */
        return BACKUP_DENY_DWELL;
    }

    /* --- Ignición --- */
    if (p->cfg.require_ignition) {
        if (!in->ignition_known) {
            if (!p->cfg.allow_unknown_ignition) {
                revoke(p, now_ms);
                return BACKUP_DENY_IGNITION_UNKNOWN;
            }
        } else if (!in->ignition_on) {
            revoke(p, now_ms);
            return BACKUP_DENY_IGNITION_OFF;
        }
    }

    /* --- Concedido --- */
    if (!p->granted) {
        p->granted = true;
        p->grants++;
        strncpy(p->granted_imei, in->imei, BACKUP_IMEI_MAX);
        p->granted_imei[BACKUP_IMEI_MAX] = '\0';
    }
    return BACKUP_OK;
}

bool backup_policy_is_granted(const backup_policy_t *p)
{
    return p && p->granted;
}

bool backup_policy_revocation_pending(const backup_policy_t *p)
{
    return p && p->revoke_pending;
}

uint32_t backup_policy_ack_revocation(backup_policy_t *p, uint64_t now_ms)
{
    if (!p || !p->revoke_pending) {
        return 0;
    }
    p->revoke_pending = false;
    if (now_ms <= p->revoked_at_ms) {
        return 0;
    }
    uint64_t d = now_ms - p->revoked_at_ms;
    return (d > UINT32_MAX) ? UINT32_MAX : (uint32_t)d;
}

const char *backup_verdict_name(backup_verdict_t v)
{
    switch (v) {
    case BACKUP_OK:                   return "permitido";
    case BACKUP_DENY_NO_IMEI:         return "sin_imei";
    case BACKUP_DENY_IMEI_CHANGED:    return "imei_cambio";
    case BACKUP_DENY_GPRS_UP:         return "gprs_arriba";
    case BACKUP_DENY_GPRS_UNKNOWN:    return "gprs_desconocido";
    case BACKUP_DENY_GPRS_STALE:      return "gprs_vencido";
    case BACKUP_DENY_DWELL:           return "dwell";
    case BACKUP_DENY_IGNITION_OFF:    return "ignicion_apagada";
    case BACKUP_DENY_IGNITION_UNKNOWN:return "ignicion_desconocida";
    default:                          return "?";
    }
}
