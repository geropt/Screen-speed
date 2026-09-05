/* Outbox de telemetría: pendiente, lote en vuelo y confirmación, separados.
 *
 * La regla que este componente existe para hacer cumplir, textual del plan:
 * **«no retirar por éxito de `send()`»**. Que el socket haya aceptado los bytes no
 * significa que el servidor los tenga: puede caerse el enlace, puede perderse el
 * ACK, puede llegar un NACK. Un outbox que borra al enviar pierde datos en silencio
 * cada vez que eso pasa.
 *
 * Por eso hay tres estados y no dos:
 *
 *   pendiente  --begin_batch-->  en vuelo  --confirm-->  (eliminado)
 *                                   |
 *                                   +--release-->  pendiente otra vez
 *
 * `confirm` es lo único que borra, y sólo se llama con un ACK del servidor en la
 * mano. `release` devuelve todo el lote a pendiente ante NACK, timeout o corte, sin
 * perder el orden.
 *
 * Un solo lote en vuelo por vez. Con varios lotes simultáneos habría que resolver
 * ACKs fuera de orden y confirmaciones parciales, y el protocolo del Ruptela no da
 * información para eso: el ACK no identifica qué lote confirma.
 *
 * Sin ESP-IDF, sin memoria dinámica, sin locks. El reloj entra por parámetro. Se
 * prueba en el host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Un record extendido del Ruptela no pasa de 126 B (RUPTELA_EXTENDED_RECORD_MAX).
 * 128 deja margen sin volver el slot enorme. */
#define OUTBOX_RECORD_MAX   128
/* Capacidad en records. 32 × 128 B = 4 KiB de estado estático, que es lo que se
 * puede sostener sin comprometer el margen de RAM interna medido en P01. */
#define OUTBOX_CAPACITY     32
/* Records por lote. El límite real lo pone el paquete del servidor; acá se acota
 * para que un lote no monopolice el enlace ni el buffer de armado. */
#define OUTBOX_BATCH_MAX    8

typedef enum {
    OUTBOX_SLOT_FREE = 0,
    OUTBOX_SLOT_PENDING,
    OUTBOX_SLOT_INFLIGHT
} outbox_slot_state_t;

typedef struct {
    outbox_slot_state_t state;
    uint16_t            len;
    uint64_t            queued_ms;
    uint8_t             bytes[OUTBOX_RECORD_MAX];
} outbox_slot_t;

typedef struct {
    /* Al llenarse, se descarta el pendiente más viejo.
     *
     * Para un respaldo de telemetría la posición más nueva vale más que la más
     * vieja: si la cola está llena es porque el enlace no funciona, y cuando
     * vuelva conviene mandar dónde está el vehículo ahora. Lo importante es que
     * el descarte se cuente, nunca que sea silencioso. Los records en vuelo NO se
     * descartan jamás. */
    bool drop_oldest_when_full;
    /* Antigüedad máxima de un pendiente. Pasada, se descarta al intentar armar un
     * lote. 0 desactiva el vencimiento. */
    uint32_t max_age_ms;
} outbox_config_t;

#define OUTBOX_CONFIG_DEFAULT() { \
    .drop_oldest_when_full = true, \
    .max_age_ms = 0,               \
}

typedef struct {
    uint32_t pushed;
    uint32_t dropped_full;      /**< descartados por cola llena          */
    uint32_t dropped_expired;   /**< descartados por antigüedad          */
    uint32_t dropped_oversize;  /**< no entraban en un slot              */
    uint32_t batches_started;
    uint32_t batches_confirmed;
    uint32_t batches_released;  /**< NACK, timeout o corte               */
    uint32_t records_confirmed;
} outbox_stats_t;

typedef struct {
    outbox_config_t cfg;
    outbox_slot_t   slots[OUTBOX_CAPACITY];
    /* Orden FIFO explícito: `seq` creciente por push. Evita depender de la
     * posición en el arreglo, que cambia al liberar slots. */
    uint64_t        seq[OUTBOX_CAPACITY];
    uint64_t        next_seq;
    /* Lote en vuelo. `batch_id` 0 significa «ninguno». */
    uint64_t        batch_id;
    uint32_t        batch_count;
    size_t          batch_bytes;
    outbox_stats_t  stats;
} outbox_t;

/** Vista de un lote: punteros a los slots en vuelo, en orden FIFO. */
typedef struct {
    uint64_t batch_id;
    uint32_t count;
    size_t   total_bytes;
    const uint8_t *records[OUTBOX_BATCH_MAX];
    uint16_t       lengths[OUTBOX_BATCH_MAX];
} outbox_batch_t;

void outbox_init(outbox_t *ob, const outbox_config_t *cfg);

/**
 * @brief Encola un record.
 *
 * @return true si quedó encolado. false si no entraba por tamaño, o si la cola
 *         estaba llena y la política no permite descartar.
 */
bool outbox_push(outbox_t *ob, uint64_t now_ms, const uint8_t *bytes, size_t len);

/** Cantidad de records esperando envío. */
uint32_t outbox_pending(const outbox_t *ob);

/** Cantidad de records entregados al transporte y todavía sin confirmar. */
uint32_t outbox_inflight(const outbox_t *ob);

/** Bytes en vuelo. El plan pide medirlos aparte de los pendientes. */
size_t outbox_inflight_bytes(const outbox_t *ob);

/**
 * @brief Marca hasta `OUTBOX_BATCH_MAX` pendientes como en vuelo.
 *
 * Descarta antes los pendientes vencidos, si hay política de antigüedad.
 *
 * @return true si se armó un lote con al menos un record. false si no hay nada
 *         pendiente o si ya hay un lote en vuelo sin resolver.
 */
bool outbox_begin_batch(outbox_t *ob, uint64_t now_ms, uint32_t max_records,
                       outbox_batch_t *out);

/**
 * @brief Confirma el lote: los records se eliminan definitivamente.
 *
 * Llamar SÓLO con un ACK del servidor. No con el retorno de `send()`.
 *
 * @return true si el id corresponde al lote en vuelo
 */
bool outbox_confirm_batch(outbox_t *ob, uint64_t batch_id);

/**
 * @brief Devuelve el lote a pendiente, conservando el orden.
 *
 * Para NACK, timeout, socket cerrado o revocación de permiso.
 *
 * @return true si el id corresponde al lote en vuelo
 */
bool outbox_release_batch(outbox_t *ob, uint64_t batch_id);

/** @return true si hay un lote esperando confirmación. */
bool outbox_has_inflight(const outbox_t *ob);

void outbox_get_stats(const outbox_t *ob, outbox_stats_t *out);

#ifdef __cplusplus
}
#endif
