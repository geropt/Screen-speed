/* Pruebas del outbox: ACK, NACK, timeout y cola llena, con un servidor simulado.
 *
 * La propiedad central que se fija es la que el plan pide textual: **no retirar por
 * éxito de `send()`**. El servidor simulado permite separar «se enviaron los bytes»
 * de «el servidor los confirmó», que es exactamente donde un outbox mal hecho pierde
 * datos en silencio.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "outbox.h"

/* ---------- servidor simulado ---------- */

typedef enum {
    SRV_ACK,        /* confirma        */
    SRV_NACK,       /* rechaza         */
    SRV_TIMEOUT,    /* no contesta     */
    SRV_DROP_LINK   /* se corta        */
} srv_behavior_t;

typedef struct {
    srv_behavior_t behavior;
    uint32_t received_batches;
    uint32_t received_records;
    size_t   received_bytes;
} fake_server_t;

/* Devuelve true si el servidor confirmó. Nótese que "enviar" siempre "funciona":
 * es justo el caso peligroso, `send()` exitoso sin confirmación. */
static bool server_exchange(fake_server_t *srv, const outbox_batch_t *batch)
{
    srv->received_batches++;
    srv->received_records += batch->count;
    srv->received_bytes += batch->total_bytes;
    return srv->behavior == SRV_ACK;
}

static void push_n(outbox_t *ob, uint64_t base_ms, int n)
{
    for (int i = 0; i < n; ++i) {
        uint8_t rec[16];
        memset(rec, 0, sizeof(rec));
        rec[0] = (uint8_t)i;
        assert(outbox_push(ob, base_ms + (uint64_t)i, rec, sizeof(rec)));
    }
}

/* ---------- casos ---------- */

static void test_estado_inicial(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    assert(outbox_pending(&ob) == 0);
    assert(outbox_inflight(&ob) == 0);
    assert(outbox_inflight_bytes(&ob) == 0);
    assert(outbox_has_inflight(&ob) == false);

    outbox_batch_t b;
    /* Sin nada pendiente no se arma lote. */
    assert(outbox_begin_batch(&ob, 1000, 4, &b) == false);
}

static void test_ack_confirma_y_libera(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 3);
    assert(outbox_pending(&ob) == 3);

    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 4, &b) == true);
    assert(b.count == 3);
    assert(b.total_bytes == 48);
    /* En vuelo, no pendiente: el dato ya salió pero todavía no está confirmado. */
    assert(outbox_pending(&ob) == 0);
    assert(outbox_inflight(&ob) == 3);
    assert(outbox_inflight_bytes(&ob) == 48);

    fake_server_t srv = { SRV_ACK, 0, 0, 0 };
    assert(server_exchange(&srv, &b) == true);
    assert(outbox_confirm_batch(&ob, b.batch_id) == true);

    assert(outbox_pending(&ob) == 0);
    assert(outbox_inflight(&ob) == 0);
    assert(outbox_has_inflight(&ob) == false);

    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.batches_confirmed == 1);
    assert(st.records_confirmed == 3);
}

static void test_send_exitoso_sin_ack_no_retira(void)
{
    /* EL caso del plan. El envío "funciona" pero el servidor no contesta: nada se
     * puede borrar, y al liberar el lote los records vuelven a pendiente. */
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 2);

    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 4, &b));

    fake_server_t srv = { SRV_TIMEOUT, 0, 0, 0 };
    bool confirmed = server_exchange(&srv, &b);
    assert(srv.received_records == 2);   /* los bytes salieron */
    assert(confirmed == false);          /* pero nadie confirmó */

    /* No se confirma. Se libera por timeout. */
    assert(outbox_release_batch(&ob, b.batch_id) == true);
    assert(outbox_pending(&ob) == 2);
    assert(outbox_inflight(&ob) == 0);

    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.batches_released == 1);
    assert(st.batches_confirmed == 0);
    assert(st.records_confirmed == 0);
}

static void test_nack_devuelve_el_lote(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 4);

    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 4, &b));
    fake_server_t srv = { SRV_NACK, 0, 0, 0 };
    assert(server_exchange(&srv, &b) == false);
    assert(outbox_release_batch(&ob, b.batch_id));
    assert(outbox_pending(&ob) == 4);

    /* Y el reintento posterior con ACK sí los saca. */
    assert(outbox_begin_batch(&ob, 3000, 4, &b));
    srv.behavior = SRV_ACK;
    assert(server_exchange(&srv, &b));
    assert(outbox_confirm_batch(&ob, b.batch_id));
    assert(outbox_pending(&ob) == 0);
}

static void test_corte_de_enlace_no_pierde_nada(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 5);

    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 8, &b));
    fake_server_t srv = { SRV_DROP_LINK, 0, 0, 0 };
    (void)server_exchange(&srv, &b);
    /* El transporte se cayó: liberar es lo correcto, no confirmar. */
    assert(outbox_release_batch(&ob, b.batch_id));
    assert(outbox_pending(&ob) == 5);
}

static void test_un_solo_lote_en_vuelo(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 10);

    outbox_batch_t b1, b2;
    assert(outbox_begin_batch(&ob, 2000, 4, &b1) == true);
    /* Segundo lote rechazado: el ACK del Ruptela no identifica qué lote confirma. */
    assert(outbox_begin_batch(&ob, 2001, 4, &b2) == false);

    assert(outbox_confirm_batch(&ob, b1.batch_id));
    /* Resuelto el primero, se puede armar el siguiente. */
    assert(outbox_begin_batch(&ob, 2002, 4, &b2) == true);
    assert(b2.count == 4);
}

static void test_orden_fifo_se_conserva_tras_release(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    /* Tres records identificables. */
    for (int i = 0; i < 3; ++i) {
        uint8_t rec[4] = { (uint8_t)(0xA0 + i), 0, 0, 0 };
        assert(outbox_push(&ob, 1000 + (uint64_t)i, rec, sizeof(rec)));
    }

    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 8, &b));
    assert(b.records[0][0] == 0xA0);
    assert(b.records[1][0] == 0xA1);
    assert(b.records[2][0] == 0xA2);

    assert(outbox_release_batch(&ob, b.batch_id));
    /* Se encola uno nuevo mientras los otros estaban en vuelo. */
    uint8_t nuevo[4] = { 0xB0, 0, 0, 0 };
    assert(outbox_push(&ob, 3000, nuevo, sizeof(nuevo)));

    /* Los liberados conservan su secuencia original: salen primero. */
    assert(outbox_begin_batch(&ob, 4000, 8, &b));
    assert(b.count == 4);
    assert(b.records[0][0] == 0xA0);
    assert(b.records[3][0] == 0xB0);
}

static void test_cola_llena_descarta_el_mas_viejo(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);   /* drop_oldest_when_full = true */

    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        uint8_t rec[4] = { (uint8_t)i, 0, 0, 0 };
        assert(outbox_push(&ob, 1000 + (uint64_t)i, rec, sizeof(rec)));
    }
    assert(outbox_pending(&ob) == OUTBOX_CAPACITY);

    /* Uno más: entra descartando el más viejo, y el descarte queda contado. */
    uint8_t extra[4] = { 0xFF, 0, 0, 0 };
    assert(outbox_push(&ob, 9000, extra, sizeof(extra)) == true);
    assert(outbox_pending(&ob) == OUTBOX_CAPACITY);

    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.dropped_full == 1);

    /* El más viejo (0) ya no está; el primero del lote es el 1. */
    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 9001, 1, &b));
    assert(b.records[0][0] == 1);
}

static void test_cola_llena_sin_descarte(void)
{
    outbox_config_t cfg = OUTBOX_CONFIG_DEFAULT();
    cfg.drop_oldest_when_full = false;
    outbox_t ob;
    outbox_init(&ob, &cfg);

    for (int i = 0; i < OUTBOX_CAPACITY; ++i) {
        uint8_t rec[4] = { (uint8_t)i, 0, 0, 0 };
        assert(outbox_push(&ob, 1000, rec, sizeof(rec)));
    }
    uint8_t extra[4] = { 0xFF, 0, 0, 0 };
    /* Con esta política se rechaza el nuevo en lugar de tirar el viejo. */
    assert(outbox_push(&ob, 2000, extra, sizeof(extra)) == false);

    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.dropped_full == 1);
}

static void test_no_se_descarta_lo_que_esta_en_vuelo(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    /* Llenar y poner TODO en vuelo. */
    for (int i = 0; i < OUTBOX_BATCH_MAX; ++i) {
        uint8_t rec[4] = { (uint8_t)i, 0, 0, 0 };
        assert(outbox_push(&ob, 1000, rec, sizeof(rec)));
    }
    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, OUTBOX_BATCH_MAX, &b));
    assert(outbox_inflight(&ob) == OUTBOX_BATCH_MAX);

    /* Llenar los slots libres restantes. */
    for (int i = 0; i < OUTBOX_CAPACITY - OUTBOX_BATCH_MAX; ++i) {
        uint8_t rec[4] = { 0x50, 0, 0, 0 };
        assert(outbox_push(&ob, 3000, rec, sizeof(rec)));
    }
    /* Ahora no hay pendientes descartables: los únicos candidatos serían los en
     * vuelo, y esos no se tocan porque puede llegar su ACK. */
    uint8_t extra[4] = { 0xFF, 0, 0, 0 };
    assert(outbox_push(&ob, 4000, extra, sizeof(extra)) == true);
    assert(outbox_inflight(&ob) == OUTBOX_BATCH_MAX);
    /* El ACK del lote original sigue siendo válido. */
    assert(outbox_confirm_batch(&ob, b.batch_id) == true);
}

static void test_vencimiento_por_antiguedad(void)
{
    outbox_config_t cfg = OUTBOX_CONFIG_DEFAULT();
    cfg.max_age_ms = 10000;
    outbox_t ob;
    outbox_init(&ob, &cfg);
    push_n(&ob, 1000, 3);

    outbox_batch_t b;
    /* 30 s después, los tres están vencidos: no se manda telemetría antigua como
     * si fuera actual. */
    assert(outbox_begin_batch(&ob, 31000, 8, &b) == false);
    assert(outbox_pending(&ob) == 0);

    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.dropped_expired == 3);
}

static void test_record_demasiado_grande(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    uint8_t big[OUTBOX_RECORD_MAX + 1];
    memset(big, 0, sizeof(big));
    /* No se trunca: medio record no es telemetría. */
    assert(outbox_push(&ob, 1000, big, sizeof(big)) == false);
    outbox_stats_t st;
    outbox_get_stats(&ob, &st);
    assert(st.dropped_oversize == 1);
    assert(outbox_pending(&ob) == 0);
}

static void test_ids_de_lote_ajenos(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, 2);
    outbox_batch_t b;
    assert(outbox_begin_batch(&ob, 2000, 4, &b));

    /* Un ACK con id que no corresponde no puede borrar nada. */
    assert(outbox_confirm_batch(&ob, b.batch_id + 999) == false);
    assert(outbox_confirm_batch(&ob, 0) == false);
    assert(outbox_release_batch(&ob, b.batch_id + 999) == false);
    assert(outbox_inflight(&ob) == 2);
    /* El correcto sí. */
    assert(outbox_confirm_batch(&ob, b.batch_id) == true);
    /* Y no se puede confirmar dos veces. */
    assert(outbox_confirm_batch(&ob, b.batch_id) == false);
}

static void test_limite_de_records_por_lote(void)
{
    outbox_t ob;
    outbox_init(&ob, NULL);
    push_n(&ob, 1000, OUTBOX_CAPACITY);

    outbox_batch_t b;
    /* Pedir más del máximo lo acota, no desborda el arreglo del lote. */
    assert(outbox_begin_batch(&ob, 2000, 9999, &b));
    assert(b.count == OUTBOX_BATCH_MAX);

    /* Y pedir 1 devuelve exactamente 1. */
    assert(outbox_confirm_batch(&ob, b.batch_id));
    assert(outbox_begin_batch(&ob, 2001, 1, &b));
    assert(b.count == 1);
}

static void test_punteros_nulos(void)
{
    outbox_init(NULL, NULL);
    assert(outbox_push(NULL, 0, NULL, 0) == false);
    assert(outbox_pending(NULL) == 0);
    assert(outbox_inflight(NULL) == 0);
    assert(outbox_inflight_bytes(NULL) == 0);
    assert(outbox_has_inflight(NULL) == false);
    assert(outbox_begin_batch(NULL, 0, 1, NULL) == false);
    assert(outbox_confirm_batch(NULL, 1) == false);
    assert(outbox_release_batch(NULL, 1) == false);
    outbox_get_stats(NULL, NULL);

    outbox_t ob;
    outbox_init(&ob, NULL);
    uint8_t rec[4] = {1, 2, 3, 4};
    assert(outbox_push(&ob, 0, NULL, 4) == false);
    assert(outbox_push(&ob, 0, rec, 0) == false);
}

int main(void)
{
    test_estado_inicial();
    test_ack_confirma_y_libera();
    test_send_exitoso_sin_ack_no_retira();
    test_nack_devuelve_el_lote();
    test_corte_de_enlace_no_pierde_nada();
    test_un_solo_lote_en_vuelo();
    test_orden_fifo_se_conserva_tras_release();
    test_cola_llena_descarta_el_mas_viejo();
    test_cola_llena_sin_descarte();
    test_no_se_descarta_lo_que_esta_en_vuelo();
    test_vencimiento_por_antiguedad();
    test_record_demasiado_grande();
    test_ids_de_lote_ajenos();
    test_limite_de_records_por_lote();
    test_punteros_nulos();
    printf("outbox tests passed\n");
    return 0;
}
