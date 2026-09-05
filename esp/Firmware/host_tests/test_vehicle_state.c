/* Pruebas de components/vehicle_state.
 *
 * Los tres casos que el plan pide por nombre para P02b están acá:
 * GPS vencido, IMEI que cambia, y «no renovar 418 porque llegó otro record».
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vehicle_state.h"

static vehicle_msg_t fix_msg(uint64_t ms, double lat, double lon, float kmh)
{
    vehicle_msg_t m = {0};
    m.kind = VEHICLE_MSG_FIX;
    m.mono_ms = ms;
    m.u.fix.lat = lat;
    m.u.fix.lon = lon;
    m.u.fix.speed_kmh = kmh;
    m.u.fix.cog_deg = 90.0f;
    return m;
}

static vehicle_msg_t ign_msg(uint64_t ms, bool on)
{
    vehicle_msg_t m = {0};
    m.kind = VEHICLE_MSG_IGNITION;
    m.mono_ms = ms;
    m.u.ignition.on = on;
    return m;
}

static vehicle_msg_t gprs_msg(uint64_t ms, bool up)
{
    vehicle_msg_t m = {0};
    m.kind = VEHICLE_MSG_GPRS;
    m.mono_ms = ms;
    m.u.gprs.up = up;
    return m;
}

static vehicle_msg_t imei_msg(uint64_t ms, const char *digits)
{
    vehicle_msg_t m = {0};
    m.kind = VEHICLE_MSG_IMEI;
    m.mono_ms = ms;
    strncpy(m.u.imei.digits, digits, VEHICLE_IMEI_MAX_LEN);
    return m;
}

/* Envoltorios: en C no se puede tomar la dirección del valor devuelto por una
 * función, así que el mensaje se materializa en una variable local. */
static bool apply_fix(vehicle_state_t *vs, uint64_t ms, double lat, double lon, float kmh)
{
    vehicle_msg_t m = fix_msg(ms, lat, lon, kmh);
    return vehicle_state_apply(vs, &m);
}

static bool apply_ign(vehicle_state_t *vs, uint64_t ms, bool on)
{
    vehicle_msg_t m = ign_msg(ms, on);
    return vehicle_state_apply(vs, &m);
}

static bool apply_gprs(vehicle_state_t *vs, uint64_t ms, bool up)
{
    vehicle_msg_t m = gprs_msg(ms, up);
    return vehicle_state_apply(vs, &m);
}

static bool apply_imei(vehicle_state_t *vs, uint64_t ms, const char *digits)
{
    vehicle_msg_t m = imei_msg(ms, digits);
    return vehicle_state_apply(vs, &m);
}

/* ---------- estado inicial ---------- */

static void test_todo_desconocido_al_arrancar(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 10000, &s);

    /* «Desconocido» no es «vencido»: nunca llegó nada. La UI tiene que poder
     * distinguirlos. */
    assert(s.fix_state == VS_UNKNOWN);
    assert(s.ignition_state == VS_UNKNOWN);
    assert(s.gprs_state == VS_UNKNOWN);
    assert(s.imei_known == false);
    assert(s.fix_age_ms == 0);
    assert(s.tracker_epoch == 1);
    assert(s.fix_epoch_current == false);
}

/* ---------- GPS vencido ---------- */

static void test_gps_vence_a_los_3s(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    assert(apply_fix(&vs, 1000, -34.6, -58.4, 60.0f) == true);

    vehicle_snapshot_t s;

    /* Recién recibido. */
    vehicle_state_snapshot(&vs, 1000, &s);
    assert(s.fix_state == VS_FRESH);
    assert(s.fix_age_ms == 0);

    /* Justo en el borde del TTL: todavía fresco. */
    vehicle_state_snapshot(&vs, 4000, &s);
    assert(s.fix_age_ms == 3000);
    assert(s.fix_state == VS_FRESH);

    /* Un milisegundo más: vencido. El valor sigue disponible, pero declarado
     * vencido; retirar la indicación de actualidad no requiere esperar bytes
     * nuevos, que es justo lo que pide el plan. */
    vehicle_state_snapshot(&vs, 4001, &s);
    assert(s.fix_state == VS_EXPIRED);
    assert(s.fix_age_ms == 3001);
    assert(s.speed_kmh == 60.0f);
    assert(s.lat == -34.6);
}

static void test_fix_nuevo_rejuvenece_solo_el_fix(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 1000, -34.6, -58.4, 60.0f);
    apply_fix(&vs, 2000, -34.7, -58.5, 61.0f);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 2500, &s);
    assert(s.fix_age_ms == 500);
    assert(s.lat == -34.7);
    assert(s.speed_kmh == 61.0f);
}

static void test_ttl_configurable(void)
{
    vehicle_state_config_t cfg = VEHICLE_STATE_CONFIG_DEFAULT();
    cfg.fix_ttl_ms = 500;
    vehicle_state_t vs;
    vehicle_state_init(&vs, &cfg);
    apply_fix(&vs, 0, 1.0, 2.0, 10.0f);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 400, &s);
    assert(s.fix_state == VS_FRESH);
    vehicle_state_snapshot(&vs, 600, &s);
    assert(s.fix_state == VS_EXPIRED);
}

/* ---------- 418 no se renueva por otro record ---------- */

static void test_418_no_se_renueva_por_otro_record(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);

    /* GPRS arriba en t=1000, y nada más de 418 después. */
    apply_gprs(&vs, 1000, true);

    /* Llegan fixes e ignición durante los 40 s siguientes: son otros records. */
    for (uint64_t t = 2000; t <= 40000; t += 1000) {
        apply_fix(&vs, t, -34.6, -58.4, 55.0f);
        apply_ign(&vs, t, true);
    }

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 40000, &s);

    /* El fix y la ignición están frescos porque llegaron. */
    assert(s.fix_state == VS_FRESH);
    assert(s.ignition_state == VS_FRESH);
    /* 418 NO se rejuveneció: su edad se cuenta desde su propio último dato y ya
     * pasó su TTL de 30 s. Esto es exactamente lo que el plan prohíbe romper. */
    assert(s.gprs_age_ms == 39000);
    assert(s.gprs_state == VS_EXPIRED);
    /* El último valor conocido sigue disponible, declarado vencido. */
    assert(s.gprs_up == true);
}

static void test_ignicion_no_se_renueva_por_fix(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_ign(&vs, 1000, true);
    for (uint64_t t = 2000; t <= 35000; t += 1000) {
        apply_fix(&vs, t, 1.0, 2.0, 10.0f);
    }
    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 35000, &s);
    assert(s.ignition_age_ms == 34000);
    assert(s.ignition_state == VS_EXPIRED);
}

static void test_imei_repetido_no_renueva_nada(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_imei(&vs, 1000, "356938035643809");
    apply_gprs(&vs, 1000, true);

    /* El marcador de IMEI se repite muchas veces; no debe tocar la edad de 418. */
    for (uint64_t t = 2000; t <= 40000; t += 1000) {
        assert(apply_imei(&vs, t, "356938035643809") == false);
    }

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 40000, &s);
    assert(s.gprs_age_ms == 39000);
    assert(s.gprs_state == VS_EXPIRED);
    /* Y no se contó como cambio de tracker. */
    assert(vs.imei_changes == 0);
    assert(s.tracker_epoch == 1);
}

/* ---------- IMEI que cambia ---------- */

static void test_imei_cambia_invalida_estado(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);

    apply_imei(&vs, 1000, "356938035643809");
    apply_fix(&vs, 1000, -34.6, -58.4, 60.0f);
    apply_ign(&vs, 1000, true);
    apply_gprs(&vs, 1000, true);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 1500, &s);
    assert(s.fix_state == VS_FRESH);
    assert(s.tracker_epoch == 1);
    assert(s.fix_epoch_current == true);

    /* Otro tracker en el mismo cable. */
    assert(apply_imei(&vs, 2000, "356938035643801") == true);

    vehicle_state_snapshot(&vs, 2000, &s);
    /* Época nueva y todo lo anterior invalidado: la posición del vehículo
     * anterior es peor que no tener posición. */
    assert(s.tracker_epoch == 2);
    assert(s.fix_state == VS_UNKNOWN);
    assert(s.ignition_state == VS_UNKNOWN);
    assert(s.gprs_state == VS_UNKNOWN);
    assert(s.fix_epoch_current == false);
    /* El IMEI nuevo sí queda registrado. */
    assert(s.imei_known == true);
    assert(strcmp(s.imei, "356938035643801") == 0);
    assert(vs.imei_changes == 1);
}

static void test_primer_imei_no_incrementa_epoca(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 1000, -34.6, -58.4, 60.0f);
    /* Conocer el IMEI por primera vez no es un cambio de tracker: no debe tirar
     * el fix que ya se tenía. */
    assert(apply_imei(&vs, 1100, "356938035643809") == true);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 1200, &s);
    assert(s.tracker_epoch == 1);
    assert(s.fix_state == VS_FRESH);
    assert(vs.imei_changes == 0);
}

static void test_epoca_marca_resultados_viejos(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_imei(&vs, 1000, "356938035643809");
    apply_fix(&vs, 1000, -34.6, -58.4, 60.0f);

    vehicle_snapshot_t antes;
    vehicle_state_snapshot(&vs, 1000, &antes);

    apply_imei(&vs, 2000, "999999999999999");

    vehicle_snapshot_t despues;
    vehicle_state_snapshot(&vs, 2000, &despues);

    /* Un resultado calculado con el snapshot viejo se puede rechazar comparando
     * la época, sin depender de tiempos. */
    assert(antes.tracker_epoch != despues.tracker_epoch);
}

/* ---------- reloj ---------- */

static void test_mensaje_reordenado_se_descarta(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 5000, -34.6, -58.4, 60.0f);
    /* El reloj es monotónico: un mensaje con marca anterior está reordenado. */
    assert(apply_fix(&vs, 4000, 0.0, 0.0, 0.0f) == false);
    assert(vs.rejected_stale_clock == 1);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 5000, &s);
    assert(s.lat == -34.6);
    assert(s.speed_kmh == 60.0f);
}

static void test_snapshot_con_reloj_atrasado_no_da_edad_negativa(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 5000, 1.0, 2.0, 3.0f);

    vehicle_snapshot_t s;
    /* No debería pasar con un reloj monotónico, pero si pasa la edad se acota a 0
     * en lugar de desbordar. */
    vehicle_state_snapshot(&vs, 4000, &s);
    assert(s.fix_age_ms == 0);
    assert(s.fix_state == VS_FRESH);
}

static void test_snapshot_es_coherente(void)
{
    /* Todas las edades salen del mismo instante: no hay forma de leer la
     * posición de un momento y la velocidad de otro. */
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 1000, -34.6, -58.4, 60.0f);
    apply_ign(&vs, 1500, true);
    apply_gprs(&vs, 1800, false);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 2000, &s);
    assert(s.mono_ms == 2000);
    assert(s.fix_age_ms == 1000);
    assert(s.ignition_age_ms == 500);
    assert(s.gprs_age_ms == 200);
}

static void test_new_epoch_explicito(void)
{
    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    apply_fix(&vs, 1000, 1.0, 2.0, 3.0f);
    vehicle_state_new_epoch(&vs);

    vehicle_snapshot_t s;
    vehicle_state_snapshot(&vs, 1000, &s);
    assert(s.tracker_epoch == 2);
    assert(s.fix_state == VS_UNKNOWN);
}

static void test_punteros_nulos(void)
{
    vehicle_state_init(NULL, NULL);
    vehicle_state_new_epoch(NULL);
    assert(vehicle_state_apply(NULL, NULL) == false);
    vehicle_state_snapshot(NULL, 0, NULL);

    vehicle_state_t vs;
    vehicle_state_init(&vs, NULL);
    assert(vehicle_state_apply(&vs, NULL) == false);
    vehicle_snapshot_t s;
    vehicle_state_snapshot(NULL, 0, &s);
    assert(s.tracker_epoch == 0);

    /* IMEI vacío no cambia nada. */
    assert(apply_imei(&vs, 100, "") == false);
}

static void test_nombres_de_estado(void)
{
    assert(strcmp(vs_state_name(VS_UNKNOWN), "desconocido") == 0);
    assert(strcmp(vs_state_name(VS_FRESH), "fresco") == 0);
    assert(strcmp(vs_state_name(VS_EXPIRED), "vencido") == 0);
}

int main(void)
{
    test_todo_desconocido_al_arrancar();
    test_gps_vence_a_los_3s();
    test_fix_nuevo_rejuvenece_solo_el_fix();
    test_ttl_configurable();
    test_418_no_se_renueva_por_otro_record();
    test_ignicion_no_se_renueva_por_fix();
    test_imei_repetido_no_renueva_nada();
    test_imei_cambia_invalida_estado();
    test_primer_imei_no_incrementa_epoca();
    test_epoca_marca_resultados_viejos();
    test_mensaje_reordenado_se_descarta();
    test_snapshot_con_reloj_atrasado_no_da_edad_negativa();
    test_snapshot_es_coherente();
    test_new_epoch_explicito();
    test_punteros_nulos();
    test_nombres_de_estado();
    printf("vehicle_state tests passed\n");
    return 0;
}
