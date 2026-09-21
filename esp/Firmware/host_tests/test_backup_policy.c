/* Pruebas de la política de permiso del respaldo.
 *
 * Lo que se fija: la ausencia de información no es permiso, el dwell evita entrar y
 * salir en cada hueco de cobertura, y la revocación es inmediata y prioritaria cuando
 * el tracker recupera GPRS.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "backup_policy.h"

static const char IMEI_A[] = "356938035643809";
static const char IMEI_B[] = "356938035643801";

static backup_inputs_t inputs(const char *imei, bool gprs_known, bool gprs_up,
                             uint32_t gprs_age, bool ign_known, bool ign_on)
{
    backup_inputs_t in;
    memset(&in, 0, sizeof(in));
    if (imei) {
        in.imei_known = true;
        strncpy(in.imei, imei, BACKUP_IMEI_MAX);
    }
    in.gprs_known = gprs_known;
    in.gprs_up = gprs_up;
    in.gprs_age_ms = gprs_age;
    in.ignition_known = ign_known;
    in.ignition_on = ign_on;
    return in;
}

/* Escenario feliz: GPRS caído, fresco, ignición encendida, dwell cumplido. */
static backup_verdict_t grant_path(backup_policy_t *p, uint64_t *t)
{
    backup_inputs_t in = inputs(IMEI_A, true, false, 0, true, true);
    /* Primera evaluación arranca el dwell. */
    backup_verdict_t v = backup_policy_evaluate(p, *t, &in);
    assert(v == BACKUP_DENY_DWELL);
    /* Pasado el dwell, se concede. */
    *t += 30000;
    return backup_policy_evaluate(p, *t, &in);
}

static void test_camino_feliz(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);
    assert(backup_policy_is_granted(&p) == true);
    assert(p.grants == 1);
}

static void test_sin_imei_no_hay_permiso(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    backup_inputs_t in = inputs(NULL, true, false, 0, true, true);
    /* Sin identidad no se puede armar un paquete válido. */
    assert(backup_policy_evaluate(&p, 100000, &in) == BACKUP_DENY_NO_IMEI);
    assert(backup_policy_is_granted(&p) == false);
}

static void test_gprs_desconocido_no_es_permiso(void)
{
    /* El caso importante: no saber si el tracker está enviando NO habilita. Si los
     * dos envían, se duplican los datos. */
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    backup_inputs_t in = inputs(IMEI_A, false, false, 0, true, true);
    assert(backup_policy_evaluate(&p, 100000, &in) == BACKUP_DENY_GPRS_UNKNOWN);
    assert(backup_policy_is_granted(&p) == false);
}

static void test_gprs_vencido_no_es_permiso(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    /* GPRS caído pero el dato tiene 90 s: no dice nada del presente. */
    backup_inputs_t in = inputs(IMEI_A, true, false, 90000, true, true);
    assert(backup_policy_evaluate(&p, 100000, &in) == BACKUP_DENY_GPRS_STALE);
}

static void test_dwell_evita_entrar_en_cada_hueco(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    backup_inputs_t down = inputs(IMEI_A, true, false, 0, true, true);
    backup_inputs_t up   = inputs(IMEI_A, true, true, 0, true, true);

    uint64_t t = 1000;
    assert(backup_policy_evaluate(&p, t, &down) == BACKUP_DENY_DWELL);
    t += 10000;
    assert(backup_policy_evaluate(&p, t, &down) == BACKUP_DENY_DWELL);
    /* Vuelve el GPRS antes de cumplir el dwell: el contador se reinicia. */
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &up) == BACKUP_DENY_GPRS_UP);
    /* Cae otra vez: hay que esperar el dwell completo de nuevo, no los 11 s ya
     * acumulados. */
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &down) == BACKUP_DENY_DWELL);
    t += 29000;
    assert(backup_policy_evaluate(&p, t, &down) == BACKUP_DENY_DWELL);
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &down) == BACKUP_OK);
}

static void test_revocacion_inmediata_al_volver_gprs(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);

    /* El tracker recupera GPRS: se revoca en la misma evaluación, sin dwell de
     * salida. Volver a competir por el IMEI es peor que perder unos records. */
    backup_inputs_t up = inputs(IMEI_A, true, true, 0, true, true);
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &up) == BACKUP_DENY_GPRS_UP);
    assert(backup_policy_is_granted(&p) == false);
    assert(backup_policy_revocation_pending(&p) == true);
    assert(p.revocations == 1);
}

static void test_revocacion_se_mide_hasta_el_cierre(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);

    backup_inputs_t up = inputs(IMEI_A, true, true, 0, true, true);
    t += 1000;
    backup_policy_evaluate(&p, t, &up);
    assert(backup_policy_revocation_pending(&p));

    /* El transporte tarda 180 ms en cerrar el socket. La meta del plan es ≤250 ms
     * para cerrar el socket local, así que la cifra tiene que ser observable. */
    uint32_t took = backup_policy_ack_revocation(&p, t + 180);
    assert(took == 180);
    assert(backup_policy_revocation_pending(&p) == false);
    /* Un segundo ack no informa nada. */
    assert(backup_policy_ack_revocation(&p, t + 500) == 0);
}

static void test_ignicion_apagada_revoca(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);

    backup_inputs_t off = inputs(IMEI_A, true, false, 0, true, false);
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &off) == BACKUP_DENY_IGNITION_OFF);
    assert(backup_policy_revocation_pending(&p) == true);
}

static void test_ignicion_desconocida_deniega_por_defecto(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    backup_inputs_t in = inputs(IMEI_A, true, false, 0, false, false);
    uint64_t t = 1000;
    assert(backup_policy_evaluate(&p, t, &in) == BACKUP_DENY_DWELL);
    t += 30000;
    /* Con la ignición desconocida no se asume que el vehículo está en viaje. */
    assert(backup_policy_evaluate(&p, t, &in) == BACKUP_DENY_IGNITION_UNKNOWN);
}

static void test_ignicion_desconocida_configurable(void)
{
    backup_policy_config_t cfg = BACKUP_POLICY_CONFIG_DEFAULT();
    cfg.allow_unknown_ignition = true;
    backup_policy_t p;
    backup_policy_init(&p, &cfg);
    backup_inputs_t in = inputs(IMEI_A, true, false, 0, false, false);
    uint64_t t = 1000;
    backup_policy_evaluate(&p, t, &in);
    t += 30000;
    assert(backup_policy_evaluate(&p, t, &in) == BACKUP_OK);
}

static void test_ignicion_no_requerida(void)
{
    backup_policy_config_t cfg = BACKUP_POLICY_CONFIG_DEFAULT();
    cfg.require_ignition = false;
    backup_policy_t p;
    backup_policy_init(&p, &cfg);
    backup_inputs_t in = inputs(IMEI_A, true, false, 0, true, false);
    uint64_t t = 1000;
    backup_policy_evaluate(&p, t, &in);
    t += 30000;
    assert(backup_policy_evaluate(&p, t, &in) == BACKUP_OK);
}

static void test_cambio_de_imei_revoca_y_no_transfiere(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);

    /* Otro tracker en el mismo cable: el permiso era para el anterior. */
    backup_inputs_t other = inputs(IMEI_B, true, false, 0, true, true);
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &other) == BACKUP_DENY_IMEI_CHANGED);
    assert(backup_policy_is_granted(&p) == false);
    assert(backup_policy_revocation_pending(&p) == true);

    /* Y el permiso al tracker nuevo exige cumplir el dwell desde cero. */
    t += 1000;
    assert(backup_policy_evaluate(&p, t, &other) == BACKUP_DENY_DWELL);
    t += 30000;
    assert(backup_policy_evaluate(&p, t, &other) == BACKUP_OK);
}

static void test_permiso_se_mantiene_mientras_corresponda(void)
{
    backup_policy_t p;
    backup_policy_init(&p, NULL);
    uint64_t t = 1000;
    assert(grant_path(&p, &t) == BACKUP_OK);
    assert(p.grants == 1);

    /* Evaluaciones repetidas no vuelven a contar como concesión nueva. */
    backup_inputs_t in = inputs(IMEI_A, true, false, 0, true, true);
    for (int i = 0; i < 10; ++i) {
        t += 1000;
        assert(backup_policy_evaluate(&p, t, &in) == BACKUP_OK);
    }
    assert(p.grants == 1);
    assert(p.revocations == 0);
}

static void test_punteros_nulos(void)
{
    backup_policy_init(NULL, NULL);
    assert(backup_policy_evaluate(NULL, 0, NULL) == BACKUP_DENY_NO_IMEI);
    assert(backup_policy_is_granted(NULL) == false);
    assert(backup_policy_revocation_pending(NULL) == false);
    assert(backup_policy_ack_revocation(NULL, 0) == 0);

    backup_policy_t p;
    backup_policy_init(&p, NULL);
    assert(backup_policy_evaluate(&p, 0, NULL) == BACKUP_DENY_NO_IMEI);
    assert(strcmp(backup_verdict_name(BACKUP_DENY_DWELL), "dwell") == 0);
}

int main(void)
{
    test_camino_feliz();
    test_sin_imei_no_hay_permiso();
    test_gprs_desconocido_no_es_permiso();
    test_gprs_vencido_no_es_permiso();
    test_dwell_evita_entrar_en_cada_hueco();
    test_revocacion_inmediata_al_volver_gprs();
    test_revocacion_se_mide_hasta_el_cierre();
    test_ignicion_apagada_revoca();
    test_ignicion_desconocida_deniega_por_defecto();
    test_ignicion_desconocida_configurable();
    test_ignicion_no_requerida();
    test_cambio_de_imei_revoca_y_no_transfiere();
    test_permiso_se_mantiene_mientras_corresponda();
    test_punteros_nulos();
    printf("backup_policy tests passed\n");
    return 0;
}
