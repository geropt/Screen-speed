/* Pairing y negociación de capacidades con el teléfono.
 *
 * Alcance de P08 en esta etapa: **contratos y lógica pura**. No hay BLE acá. Lo que
 * este componente decide es quién tiene derecho a una sesión, durante cuánto, y qué
 * puede pedir; el transporte lo implementa después, sobre la placa.
 *
 * Reglas y por qué:
 *
 *  - **Posesión única.** Un solo teléfono vinculado por vez. Abrir la ventana de
 *    pairing exige una acción física en el dispositivo, porque es la única prueba de
 *    posesión que un HUD sin teclado puede dar. Vincular uno nuevo revoca el anterior:
 *    dos dueños simultáneos no tienen forma de resolverse.
 *  - **Ventana temporal.** El pairing sólo se acepta dentro de una ventana acotada
 *    desde esa acción física. Una ventana permanentemente abierta convierte a
 *    cualquier vecino de radio en candidato.
 *  - **Límite de intentos.** Unos pocos intentos fallidos cierran la ventana. Sin
 *    esto, la ventana temporal sola no impide probar secretos en serie.
 *  - **La sesión bulk está atada al pairing.** Esto es explícito en el plan: no se
 *    expone una carga HTTP abierta «porque BLE ya autenticó otro canal». Cada sesión
 *    de transferencia lleva el id de la vinculación y un token propio; sin eso no se
 *    acepta un solo byte.
 *  - **Revocación inmediata.** Un teléfono revocado no puede reanudar una sesión en
 *    curso: el token deja de valer al instante, no al vencer.
 *  - **Capacidades explícitas.** El dispositivo declara qué versión de API soporta y
 *    qué capacidades tiene. Un cliente no puede asumir una capacidad que no fue
 *    declarada, y un cliente más nuevo que el dispositivo tiene que degradar en lugar
 *    de fallar.
 *
 * Sin ESP-IDF, sin memoria dinámica. El reloj entra por parámetro. Se prueba en host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- versión de API y capacidades ---------- */

/* Rango que este firmware soporta. El mínimo sube sólo cuando una versión vieja deja
 * de ser segura o interpretable; el máximo, cuando se agrega algo. */
#define PAIRING_API_VERSION_MIN 1
#define PAIRING_API_VERSION_MAX 1

/** Capacidades declaradas. Un bit sin declarar NO se puede asumir presente. */
typedef enum {
    PAIR_CAP_MAP_INSTALL   = 1u << 0,  /**< instalar cápsulas de mapa        */
    PAIR_CAP_OTA           = 1u << 1,  /**< actualizar firmware              */
    PAIR_CAP_BULK_BLE      = 1u << 2,  /**< transferencia por BLE            */
    PAIR_CAP_BULK_WIFI     = 1u << 3,  /**< transferencia por Wi-Fi local    */
    PAIR_CAP_DIAG_READ     = 1u << 4,  /**< leer diagnóstico sanitizado      */
    PAIR_CAP_RESUME_SWITCH = 1u << 5   /**< reanudar cambiando de transporte  */
} pairing_cap_t;

typedef struct {
    uint16_t api_version_min;
    uint16_t api_version_max;
    uint32_t capabilities;
} pairing_hello_t;

typedef struct {
    uint16_t api_version;    /**< versión acordada                     */
    uint32_t capabilities;   /**< intersección de lo que ambos soportan */
    bool     ok;
} pairing_negotiation_t;

/**
 * @brief Negocia versión y capacidades.
 *
 * Elige la versión más alta que ambos soportan. Las capacidades son la intersección:
 * lo que el dispositivo no declara, el cliente no puede usar, aunque lo pida.
 */
pairing_negotiation_t pairing_negotiate(const pairing_hello_t *device,
                                       const pairing_hello_t *client);

/* ---------- vinculación ---------- */

#define PAIRING_MAX_ATTEMPTS      3
#define PAIRING_WINDOW_MS_DEFAULT 120000   /* 2 minutos desde la acción física */
#define PAIRING_SECRET_LEN        32
#define PAIRING_TOKEN_LEN         16

typedef enum {
    PAIR_STATE_UNPAIRED = 0,   /**< sin teléfono vinculado                */
    PAIR_STATE_WINDOW,         /**< ventana abierta, esperando al cliente  */
    PAIR_STATE_PAIRED,         /**< vinculado                              */
    PAIR_STATE_LOCKED          /**< ventana cerrada por intentos fallidos  */
} pairing_state_t;

typedef enum {
    PAIR_OK = 0,
    PAIR_ERR_STATE,
    PAIR_ERR_ARG,
    PAIR_ERR_WINDOW_CLOSED,    /**< fuera de la ventana temporal           */
    PAIR_ERR_BAD_SECRET,
    PAIR_ERR_TOO_MANY,         /**< se agotaron los intentos               */
    PAIR_ERR_NOT_PAIRED,
    PAIR_ERR_WRONG_CLIENT,     /**< otro teléfono                          */
    PAIR_ERR_REVOKED,
    PAIR_ERR_API_VERSION,      /**< no hay versión en común                */
    PAIR_ERR_NO_CAP            /**< capacidad no declarada                 */
} pairing_err_t;

typedef struct {
    uint32_t window_ms;
    uint8_t  max_attempts;
} pairing_config_t;

#define PAIRING_CONFIG_DEFAULT() {        \
    .window_ms = PAIRING_WINDOW_MS_DEFAULT, \
    .max_attempts = PAIRING_MAX_ATTEMPTS,   \
}

typedef struct {
    pairing_config_t cfg;
    pairing_state_t  state;

    /* Ventana en curso. */
    uint64_t window_opened_ms;
    uint8_t  attempts;
    uint8_t  window_secret[PAIRING_SECRET_LEN];

    /* Vinculación vigente. */
    uint64_t bond_id;                       /**< 0 = ninguna              */
    uint8_t  bond_secret[PAIRING_SECRET_LEN];
    uint16_t bond_api_version;
    uint32_t bond_capabilities;

    /* Sesión bulk atada a la vinculación. */
    uint64_t session_id;                    /**< 0 = ninguna              */
    uint8_t  session_token[PAIRING_TOKEN_LEN];

    uint64_t next_id;

    /* Contadores. */
    uint32_t bonds;
    uint32_t revocations;
    uint32_t rejected_wrong_client;
    uint32_t rejected_window;
    uint32_t rejected_no_cap;
} pairing_t;

void pairing_init(pairing_t *p, const pairing_config_t *cfg);

/**
 * @brief Abre la ventana de pairing. Requiere acción física en el dispositivo.
 *
 * El secreto lo provee el llamador: generarlo con un RNG del dispositivo es su
 * responsabilidad, y este componente no inventa entropía.
 */
pairing_err_t pairing_open_window(pairing_t *p, uint64_t now_ms,
                                 const uint8_t secret[PAIRING_SECRET_LEN]);

/**
 * @brief Un cliente intenta vincularse.
 *
 * Verifica ventana, intentos, secreto y versión de API. Al lograrlo, **revoca
 * cualquier vinculación anterior**.
 */
pairing_err_t pairing_complete(pairing_t *p, uint64_t now_ms,
                              const uint8_t secret[PAIRING_SECRET_LEN],
                              const pairing_hello_t *client,
                              uint64_t *out_bond_id);

/** Cierra la ventana sin vincular. */
pairing_err_t pairing_close_window(pairing_t *p);

/** Revoca la vinculación vigente y cualquier sesión en curso. */
pairing_err_t pairing_revoke(pairing_t *p);

/**
 * @brief Abre una sesión bulk atada a la vinculación.
 *
 * @param bond_id vinculación que la pide
 * @param cap     capacidad requerida para lo que se va a hacer
 * @param out_token token que el transporte tiene que exigir en cada mensaje
 */
pairing_err_t pairing_open_session(pairing_t *p, uint64_t bond_id, pairing_cap_t cap,
                                  uint64_t *out_session_id,
                                  uint8_t out_token[PAIRING_TOKEN_LEN]);

/**
 * @brief Valida un mensaje de sesión.
 *
 * Es la comprobación que impide aceptar bytes «porque BLE ya autenticó otro canal».
 */
pairing_err_t pairing_check_session(const pairing_t *p, uint64_t session_id,
                                   const uint8_t token[PAIRING_TOKEN_LEN]);

/** Cierra la sesión bulk sin revocar la vinculación. */
pairing_err_t pairing_close_session(pairing_t *p, uint64_t session_id);

pairing_state_t pairing_state(const pairing_t *p);
bool pairing_has_capability(const pairing_t *p, pairing_cap_t cap);

const char *pairing_state_name(pairing_state_t s);
const char *pairing_err_name(pairing_err_t e);

#ifdef __cplusplus
}
#endif
