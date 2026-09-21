/* Instalador de cápsulas: inyección de fallas en cada transición persistente.
 *
 * La afirmación que el plan exige y que estas pruebas sostienen: **un corte en
 * cualquier punto no activa una candidata inválida, y la edición activa sobrevive**.
 * Sin inyectar la falla en cada punto, eso sería una intención, no un hecho.
 *
 * El almacenamiento simulado permite fallar exactamente en: reserve, write, sync,
 * verify y commit_selector. Después de cada falla se comprueba qué edición quedó
 * activa.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "map_installer.h"

/* ---------- almacenamiento simulado ---------- */

#define FAKE_CAP (1024 * 64)

typedef struct {
    uint8_t  candidate[FAKE_CAP];
    uint64_t candidate_len;
    bool     reserved;

    /* Contenido "durable" de cada edición y si es válida. */
    bool     slot_valid[3];      /* indexado por installer_slot_t */
    installer_slot_t selector;

    /* Inyección de fallas. */
    bool fail_reserve;
    bool fail_write;
    bool fail_sync;
    bool fail_verify;
    bool fail_commit;
    /* Falla sólo en la N-ésima llamada, para cortar a mitad de la transferencia. */
    int  fail_write_after;
    int  write_calls;
    int  sync_calls;

    bool discarded;
} fake_storage_t;

static bool fk_reserve(void *ctx, uint64_t bytes)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    if (st->fail_reserve || bytes > FAKE_CAP) {
        return false;
    }
    st->reserved = true;
    st->candidate_len = 0;
    memset(st->candidate, 0, sizeof(st->candidate));
    return true;
}

static bool fk_write(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    st->write_calls++;
    if (st->fail_write) {
        return false;
    }
    if (st->fail_write_after > 0 && st->write_calls > st->fail_write_after) {
        return false;
    }
    if (offset + len > FAKE_CAP) {
        return false;
    }
    memcpy(&st->candidate[offset], data, len);
    if (offset + len > st->candidate_len) {
        st->candidate_len = offset + len;
    }
    return true;
}

static bool fk_sync(void *ctx)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    st->sync_calls++;
    return !st->fail_sync;
}

static bool fk_verify(void *ctx, uint64_t total)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    if (st->fail_verify) {
        return false;
    }
    /* Verificación mínima creíble: tiene que estar todo escrito. */
    return st->candidate_len == total;
}

static bool fk_commit(void *ctx, installer_slot_t slot)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    if (st->fail_commit) {
        return false;
    }
    st->selector = slot;
    /* Al activar, esa edición pasa a contener lo recibido y es válida. */
    st->slot_valid[slot] = true;
    return true;
}

static bool fk_discard(void *ctx)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    st->discarded = true;
    st->candidate_len = 0;
    return true;
}

static installer_storage_t make_storage(fake_storage_t *st)
{
    installer_storage_t s;
    memset(&s, 0, sizeof(s));
    s.ctx = st;
    s.reserve = fk_reserve;
    s.write = fk_write;
    s.sync = fk_sync;
    s.verify = fk_verify;
    s.commit_selector = fk_commit;
    s.discard_candidate = fk_discard;
    return s;
}

/* Estado inicial: edición A activa y válida. */
static void setup(fake_storage_t *st, map_installer_t *ins)
{
    memset(st, 0, sizeof(*st));
    st->selector = INST_SLOT_A;
    st->slot_valid[INST_SLOT_A] = true;
    installer_storage_t storage = make_storage(st);
    map_installer_init(ins, &storage, INST_SLOT_A);
}

/* Transfiere `total` bytes en chunks de `chunk`. Devuelve el primer error. */
static installer_err_t transfer(map_installer_t *ins, uint64_t total, size_t chunk)
{
    uint8_t buf[256];
    uint64_t off = 0;
    while (off < total) {
        size_t n = (total - off > chunk) ? chunk : (size_t)(total - off);
        for (size_t i = 0; i < n; ++i) {
            buf[i] = (uint8_t)((off + i) & 0xFF);
        }
        installer_err_t e = map_installer_write(ins, off, buf, n);
        if (e != INST_OK) {
            return e;
        }
        off += n;
    }
    return INST_OK;
}

/* ---------- camino feliz ---------- */

static void test_instalacion_completa(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);

    assert(map_installer_begin(&ins, 1024, 7) == INST_OK);
    assert(map_installer_state(&ins) == INST_RECEIVING);
    /* La candidata es B, porque A está activa. */
    assert(ins.candidate_slot == INST_SLOT_B);

    assert(transfer(&ins, 1024, 256) == INST_OK);
    assert(map_installer_next_offset(&ins) == 1024);
    assert(map_installer_finish(&ins) == INST_OK);
    assert(map_installer_state(&ins) == INST_READY);

    assert(map_installer_activate(&ins) == INST_OK);
    assert(st.selector == INST_SLOT_B);
    assert(ins.active_slot == INST_SLOT_B);
    /* La siguiente candidata es la que estaba activa. */
    assert(ins.candidate_slot == INST_SLOT_A);
    assert(map_installer_state(&ins) == INST_IDLE);
}

/* ---------- una actualización activa por vez ---------- */

static void test_una_sesion_por_vez(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    assert(map_installer_begin(&ins, 512, 1) == INST_ERR_STATE);
}

/* ---------- inyección de fallas, transición por transición ---------- */

static void test_falla_en_reserve(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    st.fail_reserve = true;

    assert(map_installer_begin(&ins, 1024, 1) == INST_ERR_NO_SPACE);
    /* La activa no se movió. */
    assert(st.selector == INST_SLOT_A);
    assert(map_installer_activate(&ins) == INST_ERR_STATE);
}

static void test_falla_en_write(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 1024, 1) == INST_OK);
    st.fail_write = true;

    uint8_t buf[16] = {0};
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_ERR_IO);
    /* No se puede activar nada, y la activa sigue siendo A. */
    assert(map_installer_activate(&ins) == INST_ERR_STATE);
    assert(st.selector == INST_SLOT_A);
}

static void test_corte_a_mitad_de_transferencia(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 1024, 1) == INST_OK);
    /* Corte después de dos chunks: simula el enlace cayéndose. */
    st.fail_write_after = 2;

    installer_err_t e = transfer(&ins, 1024, 256);
    assert(e == INST_ERR_IO);
    /* El progreso durable informado es el de los chunks que sí se sincronizaron. */
    assert(map_installer_next_offset(&ins) == 512);
    /* Finish rechaza una candidata incompleta: no se verifica ni se activa. */
    assert(map_installer_finish(&ins) == INST_ERR_INCOMPLETE);
    assert(map_installer_activate(&ins) == INST_ERR_STATE);
    assert(st.selector == INST_SLOT_A);
}

static void test_reanudacion_desde_el_offset_durable(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 1024, 1) == INST_OK);
    st.fail_write_after = 2;
    assert(transfer(&ins, 1024, 256) == INST_ERR_IO);
    uint64_t resume = map_installer_next_offset(&ins);
    assert(resume == 512);

    /* Vuelve el enlace: se reanuda desde el offset que el instalador informó. */
    st.fail_write_after = 0;
    st.fail_write = false;
    uint8_t buf[256];
    for (uint64_t off = resume; off < 1024; off += sizeof(buf)) {
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = (uint8_t)((off + i) & 0xFF);
        }
        assert(map_installer_write(&ins, off, buf, sizeof(buf)) == INST_OK);
    }
    assert(map_installer_finish(&ins) == INST_OK);
    assert(map_installer_activate(&ins) == INST_OK);
    assert(st.selector == INST_SLOT_B);
}

static void test_falla_en_sync(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    st.fail_sync = true;

    uint8_t buf[64] = {0};
    /* Si el sync falla, el avance NO se informa como durable: el emisor reanudaría
     * desde un punto que un corte se llevaría puesto. */
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_ERR_IO);
    assert(map_installer_next_offset(&ins) == 0);
    assert(st.selector == INST_SLOT_A);
}

static void test_falla_en_verify(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    assert(transfer(&ins, 512, 128) == INST_OK);
    st.fail_verify = true;

    /* Todos los bytes llegaron, pero la cápsula no verifica: candidata inválida. */
    assert(map_installer_finish(&ins) == INST_ERR_INVALID);
    assert(map_installer_state(&ins) == INST_FAILED);
    /* Y desde FAILED no hay camino a activate. Esta es la afirmación central de la
     * etapa: una candidata inválida nunca se activa. */
    assert(map_installer_activate(&ins) == INST_ERR_STATE);
    assert(st.selector == INST_SLOT_A);
}

static void test_falla_en_commit_del_selector(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    assert(transfer(&ins, 512, 128) == INST_OK);
    assert(map_installer_finish(&ins) == INST_OK);
    st.fail_commit = true;

    assert(map_installer_activate(&ins) == INST_ERR_IO);
    /* La activa anterior sigue activa: no hay estado intermedio sin selección. */
    assert(st.selector == INST_SLOT_A);
    assert(ins.active_slot == INST_SLOT_A);
    /* La candidata verificada se conserva, así que se puede reintentar. */
    assert(map_installer_state(&ins) == INST_READY);
    st.fail_commit = false;
    assert(map_installer_activate(&ins) == INST_OK);
    assert(st.selector == INST_SLOT_B);
}

/* ---------- chunks ---------- */

static void test_chunk_reenviado_identico_es_idempotente(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);

    uint8_t buf[128];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)i;
    }
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_OK);
    /* El emisor no vio la confirmación y reenvía el mismo chunk. No es un error. */
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_OK);
    assert(map_installer_next_offset(&ins) == 128);

    installer_stats_t stats;
    map_installer_get_stats(&ins, &stats);
    assert(stats.chunks_duplicate == 1);
    assert(stats.chunks_written == 1);
}

static void test_mismo_offset_contenido_distinto_es_conflicto(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);

    uint8_t a[64], b[64];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    assert(map_installer_write(&ins, 0, a, sizeof(a)) == INST_OK);
    /* El emisor cambió de paquete a mitad de camino: eso no es reanudación. */
    assert(map_installer_write(&ins, 0, b, sizeof(b)) == INST_ERR_CONFLICT);

    installer_stats_t stats;
    map_installer_get_stats(&ins, &stats);
    assert(stats.chunks_conflict == 1);
}

static void test_hueco_se_rechaza(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    uint8_t buf[64] = {0};
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_OK);
    /* Saltar bytes dejaría una zona sin escribir que podría contener lo que había
     * antes en la tarjeta. */
    assert(map_installer_write(&ins, 200, buf, sizeof(buf)) == INST_ERR_OFFSET);
}

static void test_excede_lo_declarado(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 100, 1) == INST_OK);
    uint8_t buf[128] = {0};
    assert(map_installer_write(&ins, 0, buf, sizeof(buf)) == INST_ERR_TOO_BIG);
}

static void test_tamano_declarado_absurdo(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 0, 1) == INST_ERR_TOO_BIG);
    assert(map_installer_begin(&ins, (uint64_t)INSTALLER_MAX_TOTAL_BYTES + 1, 1)
           == INST_ERR_TOO_BIG);
}

/* ---------- cancelación ---------- */

static void test_cancelar_no_toca_la_activa(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 512, 1) == INST_OK);
    assert(transfer(&ins, 256, 128) == INST_OK);
    assert(map_installer_cancel(&ins) == INST_OK);

    assert(map_installer_state(&ins) == INST_IDLE);
    assert(st.discarded == true);
    assert(st.selector == INST_SLOT_A);
    /* Cancelar en IDLE no tiene sentido. */
    assert(map_installer_cancel(&ins) == INST_ERR_STATE);
}

static void test_cancelar_una_candidata_ya_lista(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    assert(map_installer_begin(&ins, 256, 1) == INST_OK);
    assert(transfer(&ins, 256, 256) == INST_OK);
    assert(map_installer_finish(&ins) == INST_OK);
    /* Verificada pero el usuario cancela: la activa no cambia. */
    assert(map_installer_cancel(&ins) == INST_OK);
    assert(st.selector == INST_SLOT_A);
}

/* ---------- reconciliación de arranque ---------- */

static fake_storage_t *g_recover_st;

static bool verify_slot_cb(void *ctx, installer_slot_t slot)
{
    fake_storage_t *st = (fake_storage_t *)ctx;
    return st->slot_valid[slot];
}

static void test_recover_activa_valida(void)
{
    fake_storage_t st;
    map_installer_t ins;
    setup(&st, &ins);
    g_recover_st = &st;
    assert(map_installer_recover(&ins, verify_slot_cb, &st) == INST_SLOT_A);
}

static void test_recover_vuelve_a_la_anterior(void)
{
    /* El selector quedó apuntando a B por un corte, pero B no verifica y A sí.
     * Mejor la cobertura anterior que ninguna. */
    fake_storage_t st;
    map_installer_t ins;
    memset(&st, 0, sizeof(st));
    st.selector = INST_SLOT_B;
    st.slot_valid[INST_SLOT_A] = true;
    st.slot_valid[INST_SLOT_B] = false;
    installer_storage_t storage = make_storage(&st);
    map_installer_init(&ins, &storage, INST_SLOT_B);

    assert(map_installer_recover(&ins, verify_slot_cb, &st) == INST_SLOT_A);
    /* Y el selector se reescribe para que el próximo arranque no repita el trabajo. */
    assert(st.selector == INST_SLOT_A);
}

static void test_recover_sin_ninguna_valida(void)
{
    fake_storage_t st;
    map_installer_t ins;
    memset(&st, 0, sizeof(st));
    st.selector = INST_SLOT_A;
    installer_storage_t storage = make_storage(&st);
    map_installer_init(&ins, &storage, INST_SLOT_A);

    /* Ninguna verifica: sin mapas, dicho explícitamente. Un mapa a medias daría
     * límites de velocidad de zonas equivocadas, que es peor que no tener. */
    assert(map_installer_recover(&ins, verify_slot_cb, &st) == INST_SLOT_NONE);
    assert(ins.active_slot == INST_SLOT_NONE);
}

static void test_punteros_nulos(void)
{
    map_installer_init(NULL, NULL, INST_SLOT_A);
    assert(map_installer_begin(NULL, 1, 1) == INST_ERR_ARG);
    assert(map_installer_write(NULL, 0, NULL, 0) == INST_ERR_ARG);
    assert(map_installer_finish(NULL) == INST_ERR_ARG);
    assert(map_installer_activate(NULL) == INST_ERR_ARG);
    assert(map_installer_cancel(NULL) == INST_ERR_ARG);
    assert(map_installer_next_offset(NULL) == 0);
    assert(map_installer_recover(NULL, NULL, NULL) == INST_SLOT_NONE);
    map_installer_get_stats(NULL, NULL);

    assert(strcmp(installer_state_name(INST_READY), "listo") == 0);
    assert(strcmp(installer_err_name(INST_ERR_INVALID), "invalida") == 0);
    assert(strcmp(installer_slot_name(INST_SLOT_NONE), "ninguna") == 0);
}

int main(void)
{
    test_instalacion_completa();
    test_una_sesion_por_vez();
    test_falla_en_reserve();
    test_falla_en_write();
    test_corte_a_mitad_de_transferencia();
    test_reanudacion_desde_el_offset_durable();
    test_falla_en_sync();
    test_falla_en_verify();
    test_falla_en_commit_del_selector();
    test_chunk_reenviado_identico_es_idempotente();
    test_mismo_offset_contenido_distinto_es_conflicto();
    test_hueco_se_rechaza();
    test_excede_lo_declarado();
    test_tamano_declarado_absurdo();
    test_cancelar_no_toca_la_activa();
    test_cancelar_una_candidata_ya_lista();
    test_recover_activa_valida();
    test_recover_vuelve_a_la_anterior();
    test_recover_sin_ninguna_valida();
    test_punteros_nulos();
    printf("map_installer: fault injection en todas las transiciones, tests passed\n");
    return 0;
}
