#include "map_installer.h"
#include "map_capsule.h"   /* capsule_crc32 */

#include <string.h>

static installer_slot_t other_slot(installer_slot_t s)
{
    /* Con la activa en A, la candidata es B, y viceversa. Sin activa se empieza por
     * A. La candidata nunca es la activa: eso es lo que hace que un corte deje la
     * activa intacta. */
    return (s == INST_SLOT_A) ? INST_SLOT_B : INST_SLOT_A;
}

void map_installer_init(map_installer_t *ins, const installer_storage_t *storage,
                       installer_slot_t active_slot)
{
    if (!ins) {
        return;
    }
    memset(ins, 0, sizeof(*ins));
    if (storage) {
        ins->storage = *storage;
    }
    ins->state = INST_IDLE;
    ins->active_slot = active_slot;
    ins->candidate_slot = other_slot(active_slot);
}

installer_err_t map_installer_begin(map_installer_t *ins, uint64_t total_bytes,
                                   uint32_t content_version)
{
    if (!ins) {
        return INST_ERR_ARG;
    }
    /* Una actualización activa por vez. Permitir dos sesiones simultáneas obligaría
     * a tener dos candidatas y a decidir cuál gana, sin ninguna necesidad real. */
    if (ins->state != INST_IDLE && ins->state != INST_FAILED) {
        return INST_ERR_STATE;
    }
    if (total_bytes == 0 || total_bytes > INSTALLER_MAX_TOTAL_BYTES) {
        return INST_ERR_TOO_BIG;
    }
    if (!ins->storage.reserve || !ins->storage.write || !ins->storage.sync) {
        return INST_ERR_ARG;
    }

    /* Reservar antes de recibir: descubrir que no hay lugar con la transferencia a
     * medias desperdicia el enlace y el tiempo del usuario. */
    if (!ins->storage.reserve(ins->storage.ctx, total_bytes)) {
        ins->stats.sessions_failed++;
        ins->state = INST_FAILED;
        return INST_ERR_NO_SPACE;
    }

    ins->candidate_slot = other_slot(ins->active_slot);
    ins->declared_total = total_bytes;
    ins->received = 0;
    ins->content_version = content_version;
    ins->have_last_chunk = false;
    ins->state = INST_RECEIVING;
    ins->stats.sessions_started++;
    return INST_OK;
}

installer_err_t map_installer_write(map_installer_t *ins, uint64_t offset,
                                   const uint8_t *data, size_t len)
{
    if (!ins || !data || len == 0) {
        return INST_ERR_ARG;
    }
    if (ins->state != INST_RECEIVING) {
        return INST_ERR_STATE;
    }

    uint32_t crc = capsule_crc32(data, len);

    /* Reenvío idéntico del último chunk: lo produce naturalmente una reanudación
     * después de un corte de enlace, cuando el emisor no llegó a ver la confirmación.
     * Se acepta sin volver a escribir. */
    if (ins->have_last_chunk && offset == ins->last_chunk_offset &&
        len == ins->last_chunk_len && crc == ins->last_chunk_crc) {
        ins->stats.chunks_duplicate++;
        return INST_OK;
    }

    /* Mismo offset con contenido distinto: el emisor cambió de paquete a mitad de
     * camino. Eso no es reanudación, es una sesión inconsistente. */
    if (ins->have_last_chunk && offset == ins->last_chunk_offset &&
        (len != ins->last_chunk_len || crc != ins->last_chunk_crc)) {
        ins->stats.chunks_conflict++;
        return INST_ERR_CONFLICT;
    }

    /* Sin huecos: el offset tiene que ser exactamente el próximo esperado. Aceptar
     * huecos dejaría zonas sin escribir que el verificado detectaría más tarde, y
     * peor, podrían contener lo que había antes en la tarjeta. */
    if (offset != ins->received) {
        return INST_ERR_OFFSET;
    }
    if (offset + len > ins->declared_total) {
        return INST_ERR_TOO_BIG;
    }

    if (!ins->storage.write(ins->storage.ctx, offset, data, len)) {
        return INST_ERR_IO;
    }
    /* Sync por chunk: el progreso que se le informa al emisor tiene que ser durable,
     * porque es el offset desde el que va a reanudar. Informar un avance que un corte
     * se lleva puesto haría perder datos silenciosamente. */
    if (!ins->storage.sync(ins->storage.ctx)) {
        return INST_ERR_IO;
    }

    ins->received = offset + len;
    ins->last_chunk_offset = offset;
    ins->last_chunk_len = len;
    ins->last_chunk_crc = crc;
    ins->have_last_chunk = true;
    ins->stats.chunks_written++;
    return INST_OK;
}

uint64_t map_installer_next_offset(const map_installer_t *ins)
{
    return ins ? ins->received : 0;
}

installer_state_t map_installer_state(const map_installer_t *ins)
{
    return ins ? ins->state : INST_IDLE;
}

installer_err_t map_installer_finish(map_installer_t *ins)
{
    if (!ins) {
        return INST_ERR_ARG;
    }
    if (ins->state != INST_RECEIVING) {
        return INST_ERR_STATE;
    }
    if (ins->received != ins->declared_total) {
        /* Menos bytes de los declarados: no se verifica una candidata incompleta,
         * se rechaza. */
        return INST_ERR_INCOMPLETE;
    }

    ins->state = INST_VERIFYING;
    if (!ins->storage.sync(ins->storage.ctx)) {
        ins->state = INST_FAILED;
        ins->stats.sessions_failed++;
        return INST_ERR_IO;
    }
    /* Verificación completa reabriendo con el lector real: es lo que distingue
     * «se escribieron los bytes» de «esto es una cápsula usable». */
    if (!ins->storage.verify || !ins->storage.verify(ins->storage.ctx, ins->declared_total)) {
        ins->state = INST_FAILED;
        ins->stats.sessions_failed++;
        return INST_ERR_INVALID;
    }
    ins->state = INST_READY;
    return INST_OK;
}

installer_err_t map_installer_activate(map_installer_t *ins)
{
    if (!ins) {
        return INST_ERR_ARG;
    }
    /* Sólo desde READY. Este es el punto donde «una candidata inválida nunca se
     * activa» se hace estructural: no hay camino desde RECEIVING o FAILED hasta acá. */
    if (ins->state != INST_READY) {
        return INST_ERR_STATE;
    }
    if (!ins->storage.commit_selector) {
        return INST_ERR_ARG;
    }
    if (!ins->storage.commit_selector(ins->storage.ctx, ins->candidate_slot)) {
        /* El selector no se pudo escribir: la activa anterior sigue siendo la activa.
         * No queda un estado intermedio en que nada esté seleccionado. La candidata
         * verificada se conserva, así que se puede reintentar la activación. */
        return INST_ERR_IO;
    }

    installer_slot_t previous = ins->active_slot;
    ins->active_slot = ins->candidate_slot;
    ins->candidate_slot = previous;
    ins->state = INST_IDLE;
    ins->have_last_chunk = false;
    ins->received = 0;
    ins->declared_total = 0;
    ins->stats.sessions_activated++;
    return INST_OK;
}

installer_err_t map_installer_cancel(map_installer_t *ins)
{
    if (!ins) {
        return INST_ERR_ARG;
    }
    if (ins->state == INST_IDLE) {
        return INST_ERR_STATE;
    }
    /* Se limpia sólo la candidata. La activa no se toca nunca: es la razón de que
     * cancelar sea seguro en cualquier punto. */
    if (ins->storage.discard_candidate) {
        ins->storage.discard_candidate(ins->storage.ctx);
    }
    ins->state = INST_IDLE;
    ins->received = 0;
    ins->declared_total = 0;
    ins->have_last_chunk = false;
    ins->stats.sessions_cancelled++;
    return INST_OK;
}

installer_slot_t map_installer_recover(map_installer_t *ins,
                                      bool (*verify_slot)(void *ctx, installer_slot_t),
                                      void *ctx)
{
    if (!ins || !verify_slot) {
        return INST_SLOT_NONE;
    }
    /* No se puede asumir atomicidad entre el selector y el contenido: un corte entre
     * escribir el selector y terminar de sincronizar la tarjeta deja las dos cosas
     * en desacuerdo. */
    if (ins->active_slot != INST_SLOT_NONE && verify_slot(ctx, ins->active_slot)) {
        ins->state = INST_IDLE;
        return ins->active_slot;
    }

    installer_slot_t fallback = other_slot(ins->active_slot);
    if (verify_slot(ctx, fallback)) {
        /* La seleccionada no verifica pero la anterior sí: se vuelve a esa y se
         * reescribe el selector. Mejor la cobertura anterior que ninguna. */
        if (ins->storage.commit_selector) {
            ins->storage.commit_selector(ins->storage.ctx, fallback);
        }
        ins->active_slot = fallback;
        ins->candidate_slot = other_slot(fallback);
        ins->state = INST_IDLE;
        return fallback;
    }

    /* Ninguna verifica: sin mapas, dicho explícitamente. Un mapa a medias sería
     * peor, porque daría límites de velocidad de zonas equivocadas. */
    ins->active_slot = INST_SLOT_NONE;
    ins->candidate_slot = INST_SLOT_A;
    ins->state = INST_IDLE;
    return INST_SLOT_NONE;
}

void map_installer_get_stats(const map_installer_t *ins, installer_stats_t *out)
{
    if (!ins || !out) {
        return;
    }
    *out = ins->stats;
}

const char *installer_state_name(installer_state_t s)
{
    switch (s) {
    case INST_IDLE:      return "idle";
    case INST_RECEIVING: return "recibiendo";
    case INST_VERIFYING: return "verificando";
    case INST_READY:     return "listo";
    case INST_FAILED:    return "fallido";
    default:             return "?";
    }
}

const char *installer_err_name(installer_err_t e)
{
    switch (e) {
    case INST_OK:             return "ok";
    case INST_ERR_STATE:      return "estado";
    case INST_ERR_ARG:        return "argumento";
    case INST_ERR_OFFSET:     return "offset";
    case INST_ERR_CONFLICT:   return "conflicto";
    case INST_ERR_TOO_BIG:    return "demasiado_grande";
    case INST_ERR_NO_SPACE:   return "sin_espacio";
    case INST_ERR_IO:         return "io";
    case INST_ERR_INCOMPLETE: return "incompleta";
    case INST_ERR_INVALID:    return "invalida";
    default:                  return "?";
    }
}

const char *installer_slot_name(installer_slot_t s)
{
    switch (s) {
    case INST_SLOT_A:    return "A";
    case INST_SLOT_B:    return "B";
    case INST_SLOT_NONE: return "ninguna";
    default:             return "?";
    }
}
