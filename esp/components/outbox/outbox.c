#include "outbox.h"

#include <string.h>

void outbox_init(outbox_t *ob, const outbox_config_t *cfg)
{
    if (!ob) {
        return;
    }
    memset(ob, 0, sizeof(*ob));
    if (cfg) {
        ob->cfg = *cfg;
    } else {
        outbox_config_t d = OUTBOX_CONFIG_DEFAULT();
        ob->cfg = d;
    }
    ob->next_seq = 1;
}

/* Índice del pendiente más viejo, o -1. */
static int oldest_pending(const outbox_t *ob)
{
    int best = -1;
    uint64_t best_seq = UINT64_MAX;
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_PENDING && ob->seq[i] < best_seq) {
            best_seq = ob->seq[i];
            best = i;
        }
    }
    return best;
}

static int find_free(const outbox_t *ob)
{
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

bool outbox_push(outbox_t *ob, uint64_t now_ms, const uint8_t *bytes, size_t len)
{
    if (!ob || !bytes || len == 0) {
        return false;
    }
    if (len > OUTBOX_RECORD_MAX) {
        /* No se trunca un record: la mitad de un record no es telemetría, es
         * basura que el servidor rechazaría. */
        ob->stats.dropped_oversize++;
        return false;
    }

    int idx = find_free(ob);
    if (idx < 0) {
        if (!ob->cfg.drop_oldest_when_full) {
            ob->stats.dropped_full++;
            return false;
        }
        idx = oldest_pending(ob);
        if (idx < 0) {
            /* Todos los slots están en vuelo: no se descarta nada de un lote sin
             * resolver, porque puede llegar su ACK. Se pierde el nuevo. */
            ob->stats.dropped_full++;
            return false;
        }
        ob->stats.dropped_full++;
    }

    outbox_slot_t *s = &ob->slots[idx];
    s->state = OUTBOX_SLOT_PENDING;
    s->len = (uint16_t)len;
    s->queued_ms = now_ms;
    memcpy(s->bytes, bytes, len);
    ob->seq[idx] = ob->next_seq++;
    ob->stats.pushed++;
    return true;
}

uint32_t outbox_pending(const outbox_t *ob)
{
    if (!ob) {
        return 0;
    }
    uint32_t n = 0;
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_PENDING) {
            n++;
        }
    }
    return n;
}

uint32_t outbox_inflight(const outbox_t *ob)
{
    if (!ob) {
        return 0;
    }
    uint32_t n = 0;
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_INFLIGHT) {
            n++;
        }
    }
    return n;
}

size_t outbox_inflight_bytes(const outbox_t *ob)
{
    if (!ob) {
        return 0;
    }
    size_t n = 0;
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_INFLIGHT) {
            n += ob->slots[i].len;
        }
    }
    return n;
}

bool outbox_has_inflight(const outbox_t *ob)
{
    return ob && ob->batch_id != 0;
}

static void expire_pending(outbox_t *ob, uint64_t now_ms)
{
    if (ob->cfg.max_age_ms == 0) {
        return;
    }
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state != OUTBOX_SLOT_PENDING) {
            continue;
        }
        if (now_ms > ob->slots[i].queued_ms &&
            (now_ms - ob->slots[i].queued_ms) > ob->cfg.max_age_ms) {
            ob->slots[i].state = OUTBOX_SLOT_FREE;
            ob->stats.dropped_expired++;
        }
    }
}

bool outbox_begin_batch(outbox_t *ob, uint64_t now_ms, uint32_t max_records,
                       outbox_batch_t *out)
{
    if (!ob || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* Un solo lote en vuelo: el ACK del Ruptela no identifica qué lote confirma,
     * así que con dos lotes simultáneos no habría forma de saber cuál se confirmó. */
    if (ob->batch_id != 0) {
        return false;
    }

    expire_pending(ob, now_ms);

    if (max_records == 0 || max_records > OUTBOX_BATCH_MAX) {
        max_records = OUTBOX_BATCH_MAX;
    }

    /* Selección por orden FIFO estricto: se toman los `max_records` pendientes de
     * secuencia más baja. */
    for (uint32_t taken = 0; taken < max_records; ++taken) {
        int idx = oldest_pending(ob);
        if (idx < 0) {
            break;
        }
        ob->slots[idx].state = OUTBOX_SLOT_INFLIGHT;
        out->records[out->count] = ob->slots[idx].bytes;
        out->lengths[out->count] = ob->slots[idx].len;
        out->total_bytes += ob->slots[idx].len;
        out->count++;
    }

    if (out->count == 0) {
        return false;
    }

    ob->batch_id = ob->next_seq++;
    ob->batch_count = out->count;
    ob->batch_bytes = out->total_bytes;
    out->batch_id = ob->batch_id;
    ob->stats.batches_started++;
    return true;
}

bool outbox_confirm_batch(outbox_t *ob, uint64_t batch_id)
{
    if (!ob || batch_id == 0 || ob->batch_id != batch_id) {
        return false;
    }
    uint32_t freed = 0;
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_INFLIGHT) {
            ob->slots[i].state = OUTBOX_SLOT_FREE;
            freed++;
        }
    }
    ob->batch_id = 0;
    ob->batch_count = 0;
    ob->batch_bytes = 0;
    ob->stats.batches_confirmed++;
    ob->stats.records_confirmed += freed;
    return true;
}

bool outbox_release_batch(outbox_t *ob, uint64_t batch_id)
{
    if (!ob || batch_id == 0 || ob->batch_id != batch_id) {
        return false;
    }
    /* Vuelven a pendiente con su `seq` original, así que conservan el orden y se
     * reenvían antes que lo que se encoló mientras estaban en vuelo. */
    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        if (ob->slots[i].state == OUTBOX_SLOT_INFLIGHT) {
            ob->slots[i].state = OUTBOX_SLOT_PENDING;
        }
    }
    ob->batch_id = 0;
    ob->batch_count = 0;
    ob->batch_bytes = 0;
    ob->stats.batches_released++;
    return true;
}

void outbox_get_stats(const outbox_t *ob, outbox_stats_t *out)
{
    if (!ob || !out) {
        return;
    }
    *out = ob->stats;
}
