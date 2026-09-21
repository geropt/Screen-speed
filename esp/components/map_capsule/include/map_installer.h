/* Instalador de cápsulas: máquina de estados transaccional.
 *
 * Lo que tiene que garantizar, del plan: «cortes no activan candidata inválida»,
 * «una actualización activa por vez», «recibir en candidata, reservar espacio,
 * verificar completamente y reabrir con lector real antes de marcar READY», y «no
 * asumir atomicidad NVS+SD».
 *
 * Estados:
 *
 *   IDLE --begin--> RECEIVING --finish--> VERIFYING --ok--> READY --activate--> IDLE
 *     ^                 |                     |              |
 *     +---- cancel -----+------ inválida -----+--------------+
 *
 * Ideas centrales:
 *
 *  - **La candidata y la activa son distintas.** Se recibe siempre en la candidata;
 *    la activa no se toca hasta que la candidata verificó completa. Un corte en
 *    cualquier punto deja la activa intacta.
 *  - **El selector es lo último que cambia.** Antes de tocarlo, la candidata ya está
 *    escrita, sincronizada, verificada y reabierta con el lector real. El selector es
 *    la única escritura que decide qué se usa.
 *  - **No se asume atomicidad entre NVS y SD.** Al arrancar hay que reconciliar: si
 *    el selector apunta a algo que no verifica, se vuelve a la anterior. Eso lo
 *    modela `map_installer_recover()`.
 *  - **Chunks idempotentes.** Un chunk reenviado con el mismo offset y el mismo
 *    contenido no es un error: la reanudación después de un corte de enlace lo
 *    produce naturalmente. Un chunk con el mismo offset y contenido distinto sí es un
 *    error: significa que el emisor cambió de paquete a mitad de camino.
 *  - **Paths internos.** El instalador nunca recibe una ruta del cliente. Los nombres
 *    de la candidata y de las ediciones activas los decide él.
 *
 * Sin ESP-IDF: el almacenamiento se inyecta. Se prueba en el host, incluida la
 * inyección de fallas en cada transición persistente.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSTALLER_MAX_TOTAL_BYTES (32u * 1024u * 1024u)

typedef enum {
    INST_IDLE = 0,
    INST_RECEIVING,
    INST_VERIFYING,
    INST_READY,
    INST_FAILED
} installer_state_t;

typedef enum {
    INST_OK = 0,
    INST_ERR_STATE,        /**< operación que no corresponde al estado actual   */
    INST_ERR_ARG,
    INST_ERR_OFFSET,       /**< hueco o solapamiento inconsistente               */
    INST_ERR_CONFLICT,     /**< mismo offset con contenido distinto              */
    INST_ERR_TOO_BIG,      /**< excede lo declarado o el límite                  */
    INST_ERR_NO_SPACE,     /**< el almacenamiento no pudo reservar               */
    INST_ERR_IO,           /**< escritura o sync fallidos                        */
    INST_ERR_INCOMPLETE,   /**< finish con menos bytes de los declarados         */
    INST_ERR_INVALID       /**< la candidata no verifica                         */
} installer_err_t;

/** Qué edición está seleccionada. La activa nunca es la candidata. */
typedef enum {
    INST_SLOT_NONE = 0,
    INST_SLOT_A,
    INST_SLOT_B
} installer_slot_t;

/**
 * @brief Almacenamiento inyectado.
 *
 * Todas las operaciones devuelven false ante error. Se inyectan para poder simular
 * fallas en cada transición: sin eso, «los cortes no activan una candidata inválida»
 * es una afirmación sin prueba.
 */
typedef struct {
    void *ctx;
    /** Reserva espacio para la candidata. false si no hay lugar. */
    bool (*reserve)(void *ctx, uint64_t bytes);
    /** Escribe en la candidata en un offset absoluto. */
    bool (*write)(void *ctx, uint64_t offset, const uint8_t *data, size_t len);
    /** Fuerza el vaciado a medio durable. */
    bool (*sync)(void *ctx);
    /** Verifica la candidata completa reabriéndola con el lector real. */
    bool (*verify)(void *ctx, uint64_t total_bytes);
    /** Cambia el selector durable. Es la única escritura que decide qué se usa. */
    bool (*commit_selector)(void *ctx, installer_slot_t slot);
    /** Borra el contenido de la candidata. */
    bool (*discard_candidate)(void *ctx);
} installer_storage_t;

typedef struct {
    uint32_t sessions_started;
    uint32_t sessions_activated;
    uint32_t sessions_cancelled;
    uint32_t sessions_failed;
    uint32_t chunks_written;
    uint32_t chunks_duplicate;   /**< reenvíos idénticos, aceptados             */
    uint32_t chunks_conflict;    /**< mismo offset, contenido distinto           */
} installer_stats_t;

typedef struct {
    installer_storage_t storage;
    installer_state_t   state;

    /* Sesión en curso. */
    uint64_t declared_total;
    uint64_t received;           /**< bytes contiguos confirmados durables      */
    uint32_t content_version;

    /* Selección durable. */
    installer_slot_t active_slot;
    installer_slot_t candidate_slot;

    /* Huella del último chunk, para detectar reenvíos idénticos. */
    uint64_t last_chunk_offset;
    uint32_t last_chunk_crc;
    size_t   last_chunk_len;
    bool     have_last_chunk;

    installer_stats_t stats;
} map_installer_t;

/**
 * @brief Inicializa con el estado durable ya conocido.
 *
 * @param active_slot edición activa según el selector, o INST_SLOT_NONE
 */
void map_installer_init(map_installer_t *ins, const installer_storage_t *storage,
                       installer_slot_t active_slot);

/**
 * @brief Comienza una recepción.
 *
 * Falla si ya hay una en curso: **una actualización activa por vez**. La candidata es
 * la edición que NO está activa, elegida internamente.
 */
installer_err_t map_installer_begin(map_installer_t *ins, uint64_t total_bytes,
                                   uint32_t content_version);

/**
 * @brief Escribe un chunk en un offset absoluto.
 *
 * Idempotente ante un reenvío idéntico del último chunk. Rechaza huecos: el offset
 * tiene que ser exactamente el próximo esperado.
 */
installer_err_t map_installer_write(map_installer_t *ins, uint64_t offset,
                                   const uint8_t *data, size_t len);

/** @return próximo offset recuperable; con esto el emisor reanuda. */
uint64_t map_installer_next_offset(const map_installer_t *ins);

installer_state_t map_installer_state(const map_installer_t *ins);

/**
 * @brief Cierra la recepción y verifica la candidata completa.
 *
 * Exige haber recibido exactamente lo declarado. Al verificar bien pasa a READY; al
 * fallar pasa a FAILED y la candidata queda descartable, nunca activable.
 */
installer_err_t map_installer_finish(map_installer_t *ins);

/**
 * @brief Activa la candidata verificada cambiando el selector.
 *
 * Sólo desde READY. Si el commit del selector falla, la activa anterior sigue siendo
 * la activa: no queda un estado intermedio en que nada esté seleccionado.
 */
installer_err_t map_installer_activate(map_installer_t *ins);

/** Cancela en cualquier punto. La activa no se toca. */
installer_err_t map_installer_cancel(map_installer_t *ins);

/**
 * @brief Reconciliación de arranque.
 *
 * No se puede asumir atomicidad entre el selector y el contenido. Al arrancar, se
 * verifica lo que el selector dice que está activo; si no verifica, se vuelve a la
 * otra edición si esa sí verifica, y si ninguna verifica se queda sin mapas —estado
 * explícito, no un mapa a medias—.
 *
 * @param verify_slot verifica una edición concreta
 * @return la edición que quedó activa
 */
installer_slot_t map_installer_recover(map_installer_t *ins,
                                      bool (*verify_slot)(void *ctx, installer_slot_t),
                                      void *ctx);

void map_installer_get_stats(const map_installer_t *ins, installer_stats_t *out);

const char *installer_state_name(installer_state_t s);
const char *installer_err_name(installer_err_t e);
const char *installer_slot_name(installer_slot_t s);

#ifdef __cplusplus
}
#endif
