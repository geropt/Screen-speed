/* Política de permiso del respaldo de telemetría.
 *
 * Decide si el HUD tiene derecho a enviar records al servidor en lugar del tracker.
 * Es la pieza que evita que el respaldo compita con el equipo operativo.
 *
 * Entradas y por qué cada una:
 *
 *  - **IO 418 (GPRS del tracker).** Si el tracker tiene GPRS, el respaldo no manda:
 *    duplicaría datos y ocuparía el IMEI. Es la señal principal.
 *  - **Frescura de esa señal.** Un 418 viejo no dice nada del presente. Sin dato
 *    fresco no se envía: la ausencia de información no es permiso.
 *  - **Dwell.** Hay que ver GPRS caído durante un rato continuo antes de tomar el
 *    relevo, para no entrar y salir en cada hueco de cobertura.
 *  - **Ignición.** Con el vehículo apagado no hay viaje que respaldar.
 *  - **Identidad.** Sin IMEI conocido no se puede armar un paquete válido, y con un
 *    IMEI distinto del que se venía usando hay que parar: el permiso era para ese
 *    tracker.
 *  - **Revocación.** Cuando el tracker recupera GPRS, el respaldo tiene que cesar de
 *    inmediato, con prioridad sobre cualquier envío en curso.
 *
 * LIMITACIÓN QUE HAY QUE DECIR EN VOZ ALTA. Esta política **no** es una reserva
 * exclusiva del IMEI ni detecta toda falla celular. IO 418 es lo que el tracker
 * *reporta* por el canal RS232; entre que el tracker decide enviar por GPRS y que el
 * HUD se enteró, hay una carrera que ninguna política local puede cerrar. El plan lo
 * marca explícitamente y esta implementación no lo resuelve: lo acota con dwell y
 * revocación rápida.
 *
 * Sin ESP-IDF, sin memoria dinámica. El reloj entra por parámetro.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKUP_IMEI_MAX 16

/** Por qué no se puede enviar. Una razón por vez, la más importante. */
typedef enum {
    BACKUP_OK = 0,              /**< permitido                                  */
    BACKUP_DENY_NO_IMEI,        /**< sin identidad no hay paquete válido        */
    BACKUP_DENY_IMEI_CHANGED,   /**< otro tracker: el permiso no se transfiere  */
    BACKUP_DENY_GPRS_UP,        /**< el tracker está enviando por su cuenta     */
    BACKUP_DENY_GPRS_UNKNOWN,   /**< nunca se supo el estado de GPRS            */
    BACKUP_DENY_GPRS_STALE,     /**< el dato de GPRS venció                      */
    BACKUP_DENY_DWELL,          /**< caído, pero hace poco                       */
    BACKUP_DENY_IGNITION_OFF,   /**< vehículo apagado                            */
    BACKUP_DENY_IGNITION_UNKNOWN/**< nunca se supo la ignición                   */
} backup_verdict_t;

typedef struct {
    /** Antigüedad máxima del dato de IO 418 para considerarlo utilizable. */
    uint32_t gprs_fresh_ms;
    /** Tiempo continuo con GPRS caído antes de tomar el relevo. */
    uint32_t dwell_ms;
    /** Si se exige ignición encendida. */
    bool     require_ignition;
    /** Si una ignición desconocida alcanza para permitir. */
    bool     allow_unknown_ignition;
} backup_policy_config_t;

/* Dwell de 30 s: suficiente para no entrar en cada túnel, corto frente a un corte
 * real de cobertura. Frescura de 60 s: al doble del TTL con que el estado del
 * vehículo declara vencida la señal, para que la política no sea la primera en
 * quejarse. Ambos son valores para negociar con mediciones, no medidos. */
#define BACKUP_POLICY_CONFIG_DEFAULT() { \
    .gprs_fresh_ms = 60000,              \
    .dwell_ms = 30000,                   \
    .require_ignition = true,            \
    .allow_unknown_ignition = false,     \
}

/** Entradas de la decisión, tomadas de un snapshot del estado del vehículo. */
typedef struct {
    bool     imei_known;
    char     imei[BACKUP_IMEI_MAX + 1];
    bool     gprs_known;
    bool     gprs_up;
    uint32_t gprs_age_ms;
    bool     ignition_known;
    bool     ignition_on;
} backup_inputs_t;

typedef struct {
    backup_policy_config_t cfg;
    /* IMEI con el que se concedió el permiso vigente. */
    char     granted_imei[BACKUP_IMEI_MAX + 1];
    bool     granted;
    /* Momento en que se vio GPRS caído por primera vez de forma continua.
     * 0 = no se está contando dwell. */
    uint64_t down_since_ms;
    /* Revocación pendiente de atender por el transporte. */
    bool     revoke_pending;
    uint64_t revoked_at_ms;
    /* Contadores. */
    uint32_t grants;
    uint32_t revocations;
} backup_policy_t;

void backup_policy_init(backup_policy_t *p, const backup_policy_config_t *cfg);

/**
 * @brief Evalúa la política y actualiza el estado interno.
 *
 * Debe llamarse periódicamente: el dwell se cuenta acá. Si el permiso estaba
 * concedido y deja de corresponder, deja una revocación pendiente.
 *
 * @return veredicto actual
 */
backup_verdict_t backup_policy_evaluate(backup_policy_t *p, uint64_t now_ms,
                                       const backup_inputs_t *in);

/** @return true si hay permiso vigente ahora mismo. */
bool backup_policy_is_granted(const backup_policy_t *p);

/**
 * @brief Consulta si hay una revocación que el transporte todavía no atendió.
 *
 * La revocación es prioritaria e independiente del outbox: hay que dejar de enviar
 * aunque haya un lote a medio camino.
 */
bool backup_policy_revocation_pending(const backup_policy_t *p);

/**
 * @brief El transporte declara que ya cesó los envíos.
 *
 * @param now_ms para medir cuánto tardó en cerrar
 * @return milisegundos entre la revocación y el cierre
 */
uint32_t backup_policy_ack_revocation(backup_policy_t *p, uint64_t now_ms);

const char *backup_verdict_name(backup_verdict_t v);

#ifdef __cplusplus
}
#endif
