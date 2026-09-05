/* Política de OTA A/B: qué imagen se acepta, cuándo se confirma y cuándo se vuelve.
 *
 * Alcance de P09 en esta etapa: **la lógica de decisión, pura y probada**. Nada de
 * esto flashea: escribir el slot, seleccionar el boot y reiniciar los hace el firmware
 * con `esp_ota_*`. Lo que está acá es lo que decide, que es donde viven los errores
 * caros: aceptar una imagen de otra placa, confirmar una imagen que no funciona, o
 * volver atrás por una razón que no lo justifica.
 *
 * Decisiones que este componente fija:
 *
 *  - **Verificar antes de seleccionar el boot.** Modelo, tamaño, versión, hash y firma
 *    se comprueban con la imagen ya escrita en el slot inactivo y ANTES de tocar el
 *    selector. La activa queda intacta ante cualquier rechazo.
 *  - **Autoprueba acotada, y acotada de verdad.** Ausencia de Internet, de GPS o de
 *    tarjeta **no** son fallas de la imagen: el HUD tiene que funcionar sin ninguna de
 *    las tres. Lo que sí cuenta como falla es lo interno: pánico, watchdog, no poder
 *    inicializar el panel, no poder crear las tareas.
 *  - **Anti-rollback con efecto declarado.** Rechazar versiones por debajo de un mínimo
 *    protege de reinstalar una versión vulnerable, y al mismo tiempo **inhabilita el
 *    fallback** hacia esa versión. Las dos cosas son la misma decisión y hay que verlas
 *    juntas.
 *  - **Un segundo OTA no pisa el rollback pendiente.** Mientras la imagen actual no
 *    esté confirmada, no se acepta otra: si la nueva también falla, ya no habría a
 *    dónde volver.
 *
 * Sin ESP-IDF. El reloj entra por parámetro. Se prueba en el host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_HASH_LEN       32
#define OTA_MODEL_MAX      32

typedef enum {
    OTA_SLOT_A = 0,
    OTA_SLOT_B
} ota_slot_t;

/** Estado del proceso. */
typedef enum {
    OTA_STATE_IDLE = 0,        /**< nada en curso, imagen actual confirmada    */
    OTA_STATE_RECEIVING,
    OTA_STATE_VERIFIED,        /**< candidata verificada, boot no seleccionado */
    OTA_STATE_PENDING_CONFIRM, /**< arrancó la nueva, autoprueba en curso      */
    OTA_STATE_REJECTED
} ota_state_t;

typedef enum {
    OTA_OK = 0,
    OTA_ERR_STATE,
    OTA_ERR_ARG,
    OTA_ERR_MODEL,           /**< imagen para otra placa                     */
    OTA_ERR_OVERSIZE,        /**< no cabe en el slot                          */
    OTA_ERR_HASH,            /**< el hash no coincide                         */
    OTA_ERR_SIGNATURE,       /**< firma ausente o inválida                    */
    OTA_ERR_ANTI_ROLLBACK,   /**< versión por debajo del mínimo permitido      */
    OTA_ERR_SAME_VERSION,    /**< ya está instalada                           */
    OTA_ERR_UNCONFIRMED,     /**< hay un rollback pendiente sin resolver       */
    OTA_ERR_POWER,           /**< energía insuficiente para actualizar         */
    OTA_ERR_NVS_INCOMPAT,    /**< la imagen no puede leer la configuración     */
    OTA_ERR_MAP_INCOMPAT     /**< la imagen no puede leer los mapas instalados */
} ota_err_t;

/** Descripción de la imagen candidata, tal como la declara su encabezado. */
typedef struct {
    char     model[OTA_MODEL_MAX];   /**< placa a la que corresponde       */
    uint32_t version;                /**< versión de aplicación             */
    uint32_t size_bytes;
    uint8_t  hash[OTA_HASH_LEN];
    bool     signature_present;
    bool     signature_valid;        /**< lo resuelve quien verifica la firma */
    /* Compatibilidad hacia atrás y adelante que la imagen declara. */
    uint32_t nvs_schema_min;
    uint32_t nvs_schema_max;
    uint32_t map_container_min;
    uint32_t map_container_max;
} ota_image_desc_t;

/** Contexto del dispositivo con el que se juzga la imagen. */
typedef struct {
    char     model[OTA_MODEL_MAX];
    uint32_t running_version;
    uint32_t min_allowed_version;    /**< anti-rollback                     */
    uint32_t slot_capacity;
    uint32_t nvs_schema;             /**< esquema instalado                  */
    uint32_t map_container_version;  /**< versión del contenedor de mapas    */
    bool     require_signature;
    bool     power_ok;               /**< energía suficiente para actualizar */
} ota_device_ctx_t;

/** Qué se observó durante la autoprueba. */
typedef struct {
    /* Fallas internas: sí cuentan. */
    bool panic_or_watchdog;
    bool display_init_failed;
    bool task_create_failed;
    bool uart_init_failed;
    /* Ausencias de servicios externos: NO cuentan. El HUD tiene que funcionar sin
     * ninguna de las tres, así que tomarlas como falla de la imagen produciría
     * rollbacks por estar en un estacionamiento subterráneo. */
    bool no_internet;
    bool no_gps_fix;
    bool no_sd_card;
    /* Señal positiva mínima: la pantalla mostró velocidad al menos una vez. */
    bool speed_displayed;
    uint32_t uptime_ms;
} ota_selftest_obs_t;

typedef struct {
    ota_state_t state;
    ota_slot_t  running_slot;
    ota_slot_t  candidate_slot;
    uint32_t    candidate_version;
    /* Autoprueba. */
    uint32_t    selftest_window_ms;
    bool        confirm_requires_speed;
    /* Contadores. */
    uint32_t    accepted;
    uint32_t    rejected;
    uint32_t    confirmed;
    uint32_t    rolled_back;
} ota_policy_t;

/* Ventana de autoprueba de 60 s: alcanza para arrancar, montar, enganchar el UART y
 * mostrar velocidad si hay tracker, y es corta frente al riesgo de dejar una imagen
 * sin confirmar. Valor para negociar con mediciones, no medido. */
#define OTA_SELFTEST_WINDOW_MS 60000

void ota_policy_init(ota_policy_t *p, ota_slot_t running_slot, uint32_t window_ms);

/**
 * @brief Juzga una imagen candidata.
 *
 * Se llama con la imagen ya escrita en el slot inactivo y **antes** de seleccionar el
 * boot. Cualquier rechazo deja la activa intacta.
 */
ota_err_t ota_policy_verify(ota_policy_t *p, const ota_image_desc_t *img,
                           const ota_device_ctx_t *dev,
                           const uint8_t computed_hash[OTA_HASH_LEN]);

/**
 * @brief Selecciona el boot de la candidata verificada.
 *
 * Sólo desde VERIFIED. Pasa a PENDING_CONFIRM: hasta que la autoprueba confirme, un
 * reinicio tiene que volver a la anterior.
 */
ota_err_t ota_policy_select_boot(ota_policy_t *p);

/**
 * @brief Evalúa la autoprueba.
 *
 * @return OTA_OK para confirmar, OTA_ERR_STATE si todavía no corresponde decidir, y
 *         cualquier otro error para volver atrás.
 */
ota_err_t ota_policy_evaluate_selftest(ota_policy_t *p, const ota_selftest_obs_t *obs,
                                      bool *out_should_rollback);

/** Confirma la imagen: deja de haber rollback pendiente. */
ota_err_t ota_policy_confirm(ota_policy_t *p);

/** Registra que se volvió a la imagen anterior. */
ota_err_t ota_policy_mark_rolled_back(ota_policy_t *p);

/** @return true si hay una imagen sin confirmar. */
bool ota_policy_rollback_pending(const ota_policy_t *p);

/**
 * @brief Efecto del anti-rollback sobre el fallback.
 *
 * Devuelve true si volver a `fallback_version` está permitido. Es el mismo umbral que
 * rechaza instalar una versión vieja, y hay que verlo explícitamente: subir el mínimo
 * deja sin fallback a las unidades cuya imagen anterior queda por debajo.
 */
bool ota_policy_fallback_allowed(const ota_device_ctx_t *dev, uint32_t fallback_version);

const char *ota_state_name(ota_state_t s);
const char *ota_err_name(ota_err_t e);

#ifdef __cplusplus
}
#endif
