#include "ota_policy.h"

#include <string.h>

void ota_policy_init(ota_policy_t *p, ota_slot_t running_slot, uint32_t window_ms)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->state = OTA_STATE_IDLE;
    p->running_slot = running_slot;
    p->candidate_slot = (running_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
    p->selftest_window_ms = window_ms ? window_ms : OTA_SELFTEST_WINDOW_MS;
    /* Exigir que la pantalla haya mostrado velocidad al menos una vez es la señal
     * positiva mínima de que la imagen sirve para lo que el producto hace. */
    p->confirm_requires_speed = true;
}

ota_err_t ota_policy_verify(ota_policy_t *p, const ota_image_desc_t *img,
                           const ota_device_ctx_t *dev,
                           const uint8_t computed_hash[OTA_HASH_LEN])
{
    if (!p || !img || !dev || !computed_hash) {
        return OTA_ERR_ARG;
    }

    /* Un segundo OTA no pisa un rollback pendiente: si la nueva también falla, ya no
     * habría a dónde volver. */
    if (p->state == OTA_STATE_PENDING_CONFIRM) {
        p->rejected++;
        return OTA_ERR_UNCONFIRMED;
    }
    if (p->state == OTA_STATE_VERIFIED) {
        /* Ya hay una candidata verificada esperando: no se acumulan. */
        return OTA_ERR_STATE;
    }

    /* Modelo primero: una imagen de otra placa no debe siquiera considerarse, y es el
     * error más caro porque deja el equipo sin arrancar. */
    if (strncmp(img->model, dev->model, OTA_MODEL_MAX) != 0) {
        p->rejected++;
        return OTA_ERR_MODEL;
    }
    if (img->size_bytes == 0 || img->size_bytes > dev->slot_capacity) {
        p->rejected++;
        return OTA_ERR_OVERSIZE;
    }
    /* Anti-rollback antes del hash: no tiene sentido verificar el contenido de una
     * imagen que no se va a instalar por su versión. */
    if (img->version < dev->min_allowed_version) {
        p->rejected++;
        return OTA_ERR_ANTI_ROLLBACK;
    }
    if (img->version == dev->running_version) {
        p->rejected++;
        return OTA_ERR_SAME_VERSION;
    }
    if (memcmp(img->hash, computed_hash, OTA_HASH_LEN) != 0) {
        p->rejected++;
        return OTA_ERR_HASH;
    }
    if (dev->require_signature) {
        if (!img->signature_present || !img->signature_valid) {
            p->rejected++;
            return OTA_ERR_SIGNATURE;
        }
    }
    /* Compatibilidad: la imagen nueva tiene que poder leer la configuración y los
     * mapas que ya están instalados. Si no, volver atrás no alcanzaría para recuperar
     * el equipo, porque el contenido ya estaría migrado. */
    if (dev->nvs_schema < img->nvs_schema_min || dev->nvs_schema > img->nvs_schema_max) {
        p->rejected++;
        return OTA_ERR_NVS_INCOMPAT;
    }
    if (dev->map_container_version != 0 &&
        (dev->map_container_version < img->map_container_min ||
         dev->map_container_version > img->map_container_max)) {
        p->rejected++;
        return OTA_ERR_MAP_INCOMPAT;
    }
    /* Energía al final: es la condición más volátil y no tiene sentido rechazar por
     * batería una imagen que además era inválida. */
    if (!dev->power_ok) {
        return OTA_ERR_POWER;
    }

    p->candidate_version = img->version;
    p->state = OTA_STATE_VERIFIED;
    p->accepted++;
    return OTA_OK;
}

ota_err_t ota_policy_select_boot(ota_policy_t *p)
{
    if (!p) {
        return OTA_ERR_ARG;
    }
    /* Sólo desde VERIFIED: no hay camino desde RECEIVING ni desde REJECTED hasta
     * seleccionar el boot. */
    if (p->state != OTA_STATE_VERIFIED) {
        return OTA_ERR_STATE;
    }
    p->state = OTA_STATE_PENDING_CONFIRM;
    return OTA_OK;
}

ota_err_t ota_policy_evaluate_selftest(ota_policy_t *p, const ota_selftest_obs_t *obs,
                                      bool *out_should_rollback)
{
    if (!p || !obs || !out_should_rollback) {
        return OTA_ERR_ARG;
    }
    *out_should_rollback = false;
    if (p->state != OTA_STATE_PENDING_CONFIRM) {
        return OTA_ERR_STATE;
    }

    /* Fallas internas: la imagen no funciona. Volver atrás. */
    if (obs->panic_or_watchdog || obs->display_init_failed ||
        obs->task_create_failed || obs->uart_init_failed) {
        *out_should_rollback = true;
        return OTA_ERR_STATE;
    }

    /* Ausencia de servicios externos: NO es falla de la imagen. El HUD tiene que
     * funcionar sin Internet, sin fix de GPS y sin tarjeta; tomar cualquiera de esas
     * como falla produciría rollbacks por estar en un estacionamiento subterráneo.
     * Se ignoran explícitamente. */
    (void)obs->no_internet;
    (void)obs->no_gps_fix;
    (void)obs->no_sd_card;

    if (obs->uptime_ms < p->selftest_window_ms) {
        /* Todavía no corresponde decidir: la ventana no terminó. */
        return OTA_ERR_STATE;
    }

    if (p->confirm_requires_speed && !obs->speed_displayed) {
        /* Pasó la ventana sin mostrar velocidad ni una vez. Con tracker presente eso
         * es una imagen que no sirve; sin tracker, no hay forma de distinguirlo desde
         * acá, así que se prefiere volver atrás.
         *
         * ESTA ES UNA DECISIÓN DISCUTIBLE y queda anotada como tal: en una unidad
         * instalada sin tracker conectado provocaría un rollback innecesario. Si eso
         * resulta un caso real, hay que exigir además haber visto bytes en el UART. */
        *out_should_rollback = true;
        return OTA_ERR_STATE;
    }

    return OTA_OK;
}

ota_err_t ota_policy_confirm(ota_policy_t *p)
{
    if (!p) {
        return OTA_ERR_ARG;
    }
    if (p->state != OTA_STATE_PENDING_CONFIRM) {
        return OTA_ERR_STATE;
    }
    /* Confirmada: la que corría pasa a ser la candidata del próximo OTA. */
    ota_slot_t previous = p->running_slot;
    p->running_slot = p->candidate_slot;
    p->candidate_slot = previous;
    p->state = OTA_STATE_IDLE;
    p->candidate_version = 0;
    p->confirmed++;
    return OTA_OK;
}

ota_err_t ota_policy_mark_rolled_back(ota_policy_t *p)
{
    if (!p) {
        return OTA_ERR_ARG;
    }
    if (p->state != OTA_STATE_PENDING_CONFIRM) {
        return OTA_ERR_STATE;
    }
    /* Se volvió: el slot que corría sigue siendo el mismo y la candidata queda
     * disponible para sobrescribir. */
    p->state = OTA_STATE_IDLE;
    p->candidate_version = 0;
    p->rolled_back++;
    return OTA_OK;
}

bool ota_policy_rollback_pending(const ota_policy_t *p)
{
    return p && p->state == OTA_STATE_PENDING_CONFIRM;
}

bool ota_policy_fallback_allowed(const ota_device_ctx_t *dev, uint32_t fallback_version)
{
    if (!dev) {
        return false;
    }
    /* Mismo umbral que rechaza instalar: subir el mínimo deja sin fallback a las
     * unidades cuya imagen anterior queda por debajo. Es una consecuencia, no un
     * efecto secundario. */
    return fallback_version >= dev->min_allowed_version;
}

const char *ota_state_name(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_IDLE:            return "idle";
    case OTA_STATE_RECEIVING:       return "recibiendo";
    case OTA_STATE_VERIFIED:        return "verificada";
    case OTA_STATE_PENDING_CONFIRM: return "sin_confirmar";
    case OTA_STATE_REJECTED:        return "rechazada";
    default:                        return "?";
    }
}

const char *ota_err_name(ota_err_t e)
{
    switch (e) {
    case OTA_OK:                 return "ok";
    case OTA_ERR_STATE:          return "estado";
    case OTA_ERR_ARG:            return "argumento";
    case OTA_ERR_MODEL:          return "modelo_ajeno";
    case OTA_ERR_OVERSIZE:       return "no_cabe";
    case OTA_ERR_HASH:           return "hash";
    case OTA_ERR_SIGNATURE:      return "firma";
    case OTA_ERR_ANTI_ROLLBACK:  return "anti_rollback";
    case OTA_ERR_SAME_VERSION:   return "misma_version";
    case OTA_ERR_UNCONFIRMED:    return "sin_confirmar";
    case OTA_ERR_POWER:          return "energia";
    case OTA_ERR_NVS_INCOMPAT:   return "nvs_incompatible";
    case OTA_ERR_MAP_INCOMPAT:   return "mapa_incompatible";
    default:                     return "?";
    }
}
