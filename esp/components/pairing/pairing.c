#include "pairing.h"

#include <string.h>

/* Comparación en tiempo constante. Con un memcmp común, el tiempo de respuesta
 * filtra cuántos bytes coincidieron, y con ventana e intentos limitados eso sigue
 * siendo información que no hay razón para regalar. */
static bool secret_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

pairing_negotiation_t pairing_negotiate(const pairing_hello_t *device,
                                       const pairing_hello_t *client)
{
    pairing_negotiation_t r;
    memset(&r, 0, sizeof(r));
    if (!device || !client) {
        return r;
    }

    /* Rango en común. Si no hay superposición no se negocia una versión "parecida":
     * interpretar un protocolo que no se conoce es peor que no hablar. */
    uint16_t lo = (device->api_version_min > client->api_version_min)
                      ? device->api_version_min : client->api_version_min;
    uint16_t hi = (device->api_version_max < client->api_version_max)
                      ? device->api_version_max : client->api_version_max;
    if (lo > hi) {
        r.ok = false;
        return r;
    }

    /* La más alta que ambos soportan: un cliente más nuevo degrada, no falla. */
    r.api_version = hi;
    /* Intersección: lo que el dispositivo no declara, el cliente no puede usar. */
    r.capabilities = device->capabilities & client->capabilities;
    r.ok = true;
    return r;
}

void pairing_init(pairing_t *p, const pairing_config_t *cfg)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    if (cfg) {
        p->cfg = *cfg;
    } else {
        pairing_config_t d = PAIRING_CONFIG_DEFAULT();
        p->cfg = d;
    }
    p->state = PAIR_STATE_UNPAIRED;
    p->next_id = 1;
}

pairing_err_t pairing_open_window(pairing_t *p, uint64_t now_ms,
                                 const uint8_t secret[PAIRING_SECRET_LEN])
{
    if (!p || !secret) {
        return PAIR_ERR_ARG;
    }
    /* Se puede reabrir desde cualquier estado: la acción física en el dispositivo es
     * la autoridad, y es lo que permite recuperar el control tras perder el teléfono
     * o quedar bloqueado por intentos. */
    p->state = PAIR_STATE_WINDOW;
    p->window_opened_ms = now_ms;
    p->attempts = 0;
    memcpy(p->window_secret, secret, PAIRING_SECRET_LEN);
    return PAIR_OK;
}

pairing_err_t pairing_close_window(pairing_t *p)
{
    if (!p) {
        return PAIR_ERR_ARG;
    }
    if (p->state != PAIR_STATE_WINDOW && p->state != PAIR_STATE_LOCKED) {
        return PAIR_ERR_STATE;
    }
    memset(p->window_secret, 0, PAIRING_SECRET_LEN);
    p->state = (p->bond_id != 0) ? PAIR_STATE_PAIRED : PAIR_STATE_UNPAIRED;
    return PAIR_OK;
}

pairing_err_t pairing_complete(pairing_t *p, uint64_t now_ms,
                              const uint8_t secret[PAIRING_SECRET_LEN],
                              const pairing_hello_t *client,
                              uint64_t *out_bond_id)
{
    if (!p || !secret || !client) {
        return PAIR_ERR_ARG;
    }
    if (p->state == PAIR_STATE_LOCKED) {
        return PAIR_ERR_TOO_MANY;
    }
    if (p->state != PAIR_STATE_WINDOW) {
        return PAIR_ERR_STATE;
    }
    /* Ventana temporal: fuera de ella no se acepta, aunque el secreto sea correcto. */
    if (now_ms < p->window_opened_ms ||
        (now_ms - p->window_opened_ms) > p->cfg.window_ms) {
        p->rejected_window++;
        return PAIR_ERR_WINDOW_CLOSED;
    }

    if (!secret_equal(secret, p->window_secret, PAIRING_SECRET_LEN)) {
        p->attempts++;
        if (p->attempts >= p->cfg.max_attempts) {
            /* Se agotaron los intentos: la ventana se cierra y hace falta otra
             * acción física. La ventana temporal sola no impide probar secretos en
             * serie; el límite de intentos sí. */
            p->state = PAIR_STATE_LOCKED;
            memset(p->window_secret, 0, PAIRING_SECRET_LEN);
            return PAIR_ERR_TOO_MANY;
        }
        return PAIR_ERR_BAD_SECRET;
    }

    pairing_hello_t device = {
        .api_version_min = PAIRING_API_VERSION_MIN,
        .api_version_max = PAIRING_API_VERSION_MAX,
        /* Capacidades que este firmware declara hoy. BULK_WIFI y RESUME_SWITCH no
         * están: no hay implementación detrás, y declarar una capacidad que no existe
         * haría que el cliente la use y falle. */
        .capabilities = PAIR_CAP_MAP_INSTALL | PAIR_CAP_OTA |
                        PAIR_CAP_BULK_BLE | PAIR_CAP_DIAG_READ,
    };
    pairing_negotiation_t neg = pairing_negotiate(&device, client);
    if (!neg.ok) {
        return PAIR_ERR_API_VERSION;
    }

    /* Posesión única: vincular uno nuevo revoca el anterior y cualquier sesión que
     * tuviera abierta. Dos dueños simultáneos no tienen forma de resolverse. */
    if (p->bond_id != 0) {
        p->revocations++;
    }
    p->session_id = 0;
    memset(p->session_token, 0, PAIRING_TOKEN_LEN);

    p->bond_id = p->next_id++;
    memcpy(p->bond_secret, secret, PAIRING_SECRET_LEN);
    p->bond_api_version = neg.api_version;
    p->bond_capabilities = neg.capabilities;
    p->state = PAIR_STATE_PAIRED;
    p->bonds++;
    memset(p->window_secret, 0, PAIRING_SECRET_LEN);

    if (out_bond_id) {
        *out_bond_id = p->bond_id;
    }
    return PAIR_OK;
}

pairing_err_t pairing_revoke(pairing_t *p)
{
    if (!p) {
        return PAIR_ERR_ARG;
    }
    if (p->bond_id == 0) {
        return PAIR_ERR_NOT_PAIRED;
    }
    /* Inmediata: el token deja de valer ahora, no al vencer. Una revocación que
     * espera un vencimiento deja al teléfono perdido operando mientras tanto. */
    p->bond_id = 0;
    memset(p->bond_secret, 0, PAIRING_SECRET_LEN);
    p->bond_capabilities = 0;
    p->bond_api_version = 0;
    p->session_id = 0;
    memset(p->session_token, 0, PAIRING_TOKEN_LEN);
    p->state = PAIR_STATE_UNPAIRED;
    p->revocations++;
    return PAIR_OK;
}

pairing_err_t pairing_open_session(pairing_t *p, uint64_t bond_id, pairing_cap_t cap,
                                  uint64_t *out_session_id,
                                  uint8_t out_token[PAIRING_TOKEN_LEN])
{
    if (!p || !out_session_id || !out_token) {
        return PAIR_ERR_ARG;
    }
    if (p->state != PAIR_STATE_PAIRED || p->bond_id == 0) {
        return PAIR_ERR_NOT_PAIRED;
    }
    if (bond_id != p->bond_id) {
        /* Otro teléfono, o el mismo con una vinculación vieja. */
        p->rejected_wrong_client++;
        return PAIR_ERR_WRONG_CLIENT;
    }
    if ((p->bond_capabilities & (uint32_t)cap) == 0) {
        /* La capacidad no fue declarada ni acordada: no se concede una sesión para
         * algo que el dispositivo no ofrece. */
        p->rejected_no_cap++;
        return PAIR_ERR_NO_CAP;
    }

    p->session_id = p->next_id++;
    /* El token se deriva del id y del secreto de la vinculación. NO es criptografía:
     * es la ligadura que exige el plan entre la sesión bulk y el pairing, para que no
     * exista un canal aceptando bytes sólo porque otro canal autenticó. Derivarlo con
     * un MAC de verdad es parte de P10. */
    for (size_t i = 0; i < PAIRING_TOKEN_LEN; ++i) {
        p->session_token[i] = (uint8_t)(p->bond_secret[i] ^
                                        (uint8_t)((p->session_id >> (i % 8 * 8)) & 0xFF) ^
                                        (uint8_t)(i * 31u));
    }
    *out_session_id = p->session_id;
    memcpy(out_token, p->session_token, PAIRING_TOKEN_LEN);
    return PAIR_OK;
}

pairing_err_t pairing_check_session(const pairing_t *p, uint64_t session_id,
                                   const uint8_t token[PAIRING_TOKEN_LEN])
{
    if (!p || !token) {
        return PAIR_ERR_ARG;
    }
    if (p->bond_id == 0) {
        /* Revocado: el token no vale más, aunque la sesión estuviera a mitad de una
         * transferencia. */
        return PAIR_ERR_REVOKED;
    }
    if (p->session_id == 0 || session_id != p->session_id) {
        return PAIR_ERR_STATE;
    }
    if (!secret_equal(token, p->session_token, PAIRING_TOKEN_LEN)) {
        return PAIR_ERR_WRONG_CLIENT;
    }
    return PAIR_OK;
}

pairing_err_t pairing_close_session(pairing_t *p, uint64_t session_id)
{
    if (!p) {
        return PAIR_ERR_ARG;
    }
    if (p->session_id == 0 || session_id != p->session_id) {
        return PAIR_ERR_STATE;
    }
    p->session_id = 0;
    memset(p->session_token, 0, PAIRING_TOKEN_LEN);
    return PAIR_OK;
}

pairing_state_t pairing_state(const pairing_t *p)
{
    return p ? p->state : PAIR_STATE_UNPAIRED;
}

bool pairing_has_capability(const pairing_t *p, pairing_cap_t cap)
{
    return p && p->bond_id != 0 && (p->bond_capabilities & (uint32_t)cap) != 0;
}

const char *pairing_state_name(pairing_state_t s)
{
    switch (s) {
    case PAIR_STATE_UNPAIRED: return "sin_vincular";
    case PAIR_STATE_WINDOW:   return "ventana";
    case PAIR_STATE_PAIRED:   return "vinculado";
    case PAIR_STATE_LOCKED:   return "bloqueado";
    default:                  return "?";
    }
}

const char *pairing_err_name(pairing_err_t e)
{
    switch (e) {
    case PAIR_OK:                 return "ok";
    case PAIR_ERR_STATE:          return "estado";
    case PAIR_ERR_ARG:            return "argumento";
    case PAIR_ERR_WINDOW_CLOSED:  return "ventana_cerrada";
    case PAIR_ERR_BAD_SECRET:     return "secreto_malo";
    case PAIR_ERR_TOO_MANY:       return "demasiados_intentos";
    case PAIR_ERR_NOT_PAIRED:     return "sin_vincular";
    case PAIR_ERR_WRONG_CLIENT:   return "cliente_ajeno";
    case PAIR_ERR_REVOKED:        return "revocado";
    case PAIR_ERR_API_VERSION:    return "version_api";
    case PAIR_ERR_NO_CAP:         return "capacidad_no_declarada";
    default:                      return "?";
    }
}
