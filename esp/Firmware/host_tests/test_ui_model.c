/* Pruebas de components/ui_model: la lógica de presentación del HUD.
 *
 * Lo que se fija acá es que la pantalla no miente: velocidad visible en cuanto hay
 * fix válido, sin esperar el matching; cuatro estados distintos de límite; ninguna
 * alerta sobre datos declarados no actuales; y ningún límite antes de haber leído
 * un tile.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui_model.h"

/* ---------- utilidades ---------- */

static vehicle_snapshot_t snap_of(uint64_t now_ms, vs_state_t fix_state,
                                 float speed_kmh, uint64_t epoch)
{
    vehicle_snapshot_t s = {0};
    s.mono_ms = now_ms;
    s.tracker_epoch = epoch;
    s.fix_state = fix_state;
    s.speed_kmh = speed_kmh;
    s.lat = -34.6;
    s.lon = -58.4;
    s.fix_epoch_current = (fix_state != VS_UNKNOWN);
    s.ignition_state = VS_UNKNOWN;
    s.gprs_state = VS_UNKNOWN;
    return s;
}

static ui_match_input_t match_of(int32_t limit, const char *street, uint64_t epoch)
{
    ui_match_input_t m = {0};
    m.matched = true;
    m.has_limit = (limit > 0);
    m.limit_kmh = limit;
    m.anonymous = (street == NULL);
    m.tracker_epoch = epoch;
    if (street) {
        strncpy(m.street, street, UI_STREET_MAX - 1);
    }
    return m;
}

static void feed_match(ui_model_state_t *st, uint64_t now_ms, int32_t limit,
                      const char *street, uint64_t epoch)
{
    ui_match_input_t m = match_of(limit, street, epoch);
    ui_model_on_match(st, now_ms, &m, epoch);
}

/* ---------- límite inicial ficticio ---------- */

static void test_sin_limite_al_arrancar(void)
{
    /* El bug que P03 debe cerrar: `vars.c` arrancaba con speed_limit = 78 y la
     * pantalla mostraba ese número antes de leer un solo tile. */
    ui_model_state_t st;
    ui_model_init(&st, NULL);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 40.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    assert(m.limit_state == UI_LIMIT_UNKNOWN);
    assert(m.limit_kmh == 0);
    /* Y sin límite no puede haber alerta de exceso. */
    assert(m.overspeed == false);
}

/* ---------- velocidad independiente del matching ---------- */

static void test_velocidad_visible_antes_del_matching(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);

    /* Primer fix válido y ningún matching todavía. */
    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 62.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    assert(m.speed_state == UI_SPEED_LIVE);
    assert(m.speed_kmh == 62);
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
}

static void test_velocidad_sin_tarjeta(void)
{
    /* El velocímetro no depende de los mapas: con SD ausente sigue mostrando. */
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 55.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, false, &m);

    assert(m.speed_state == UI_SPEED_LIVE);
    assert(m.speed_kmh == 55);
    assert(m.sd_present == false);
    assert(m.screen == UI_SCREEN_NO_MAPS);
}

static void test_velocidad_desconocida_y_vencida(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    ui_model_t m;

    /* Nunca llegó un fix. */
    vehicle_snapshot_t s0 = snap_of(1000, VS_UNKNOWN, 0.0f, 1);
    ui_model_build(&st, &s0, true, &m);
    assert(m.speed_state == UI_SPEED_UNKNOWN);
    assert(m.screen == UI_SCREEN_WAITING_LINK);

    /* Llegó y venció: el número sigue, declarado no actual. */
    vehicle_snapshot_t s1 = snap_of(9000, VS_EXPIRED, 48.0f, 1);
    ui_model_build(&st, &s1, true, &m);
    assert(m.speed_state == UI_SPEED_STALE);
    assert(m.speed_kmh == 48);
}

/* ---------- cuatro estados de límite ---------- */

static void test_limite_conocido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    assert(m.limit_state == UI_LIMIT_KNOWN);
    assert(m.limit_kmh == 40);
    assert(m.show_street == true);
    assert(strcmp(m.street, "Lavalle") == 0);
}

static void test_limite_ultimo_conocido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    /* Pasa el tiempo sin que el matcher vuelva a confirmar el tramo. */
    ui_model_on_no_match(&st, 5000);
    vehicle_snapshot_t snap = snap_of(5000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    /* Se conserva —el cliente lo pidió— pero declarado como último conocido, no
     * como límite del tramo actual. */
    assert(m.limit_state == UI_LIMIT_LAST_KNOWN);
    assert(m.limit_kmh == 40);
}

static void test_limite_vencido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    /* Más allá del TTL de 30 s sin confirmación. */
    vehicle_snapshot_t snap = snap_of(40000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    assert(m.limit_state == UI_LIMIT_EXPIRED);
    assert(m.limit_kmh == 40);
}

static void test_limite_inferido_en_via_anonima(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    /* street == NULL marca vía sin nombre. */
    feed_match(&st, 1000, 40, NULL, 1);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    assert(m.limit_state == UI_LIMIT_INFERRED);
    assert(m.limit_kmh == 40);
    /* Sin nombre de vía no se muestra calle. */
    assert(m.show_street == false);
}

static void test_tramo_sin_limite_no_inventa(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    /* Tramo resuelto pero sin maxspeed en OSM: limit_kmh 0. */
    feed_match(&st, 1000, 0, "Pasaje sin datos", 1);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);

    /* No se inventa un límite: sigue desconocido. La calle sí se puede mostrar. */
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
    assert(m.limit_kmh == 0);
    assert(m.show_street == true);
}

static void test_limite_conserva_valor_si_tramo_nuevo_no_declara(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 60, "Avenida", 1);
    /* Tramo siguiente sin límite declarado: se conserva el anterior. */
    feed_match(&st, 2000, 0, "Avenida", 1);

    vehicle_snapshot_t snap = snap_of(2000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_KNOWN);
    assert(m.limit_kmh == 60);
}

/* ---------- histéresis de la alerta ---------- */

static void test_histeresis_enciende_y_apaga(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);   /* on = +5, off = +2 */
    feed_match(&st, 1000, 40, "Lavalle", 1);
    ui_model_t m;

    /* 44 km/h con límite 40: por debajo del umbral de encendido (45). */
    vehicle_snapshot_t s1 = snap_of(1000, VS_FRESH, 44.0f, 1);
    ui_model_build(&st, &s1, true, &m);
    assert(m.overspeed == false);

    /* 46: supera 40+5, enciende. */
    vehicle_snapshot_t s2 = snap_of(1000, VS_FRESH, 46.0f, 1);
    ui_model_build(&st, &s2, true, &m);
    assert(m.overspeed == true);

    /* 44: ya encendida, no se apaga todavía (umbral de apagado es 42). Esto es lo
     * que evita el titileo cuando la velocidad oscila sobre el límite. */
    ui_model_build(&st, &s1, true, &m);
    assert(m.overspeed == true);

    /* 41: baja del umbral de apagado, se apaga. */
    vehicle_snapshot_t s3 = snap_of(1000, VS_FRESH, 41.0f, 1);
    ui_model_build(&st, &s3, true, &m);
    assert(m.overspeed == false);
}

static void test_no_alerta_con_fix_vencido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    /* Velocidad alta pero el fix venció: no se alerta sobre un dato que se
     * declara no actual. */
    vehicle_snapshot_t snap = snap_of(2000, VS_EXPIRED, 90.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.speed_state == UI_SPEED_STALE);
    assert(m.overspeed == false);
}

static void test_no_alerta_con_limite_vencido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    vehicle_snapshot_t snap = snap_of(40000, VS_FRESH, 90.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_EXPIRED);
    assert(m.overspeed == false);
}

static void test_no_alerta_a_paso_de_hombre(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 1, "Peatonal", 1);

    /* Límite 1 km/h y velocidad 3: superaría el umbral, pero por debajo de
     * alert_min_speed_kmh no se evalúa exceso. */
    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 3.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.overspeed == false);
}

static void test_alerta_con_limite_inferido(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, NULL, 1);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 60.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_INFERRED);
    /* Un límite declarado como inferencia igual sirve para alertar. */
    assert(m.overspeed == true);
}

static void test_umbrales_configurables(void)
{
    ui_model_config_t cfg = UI_MODEL_CONFIG_DEFAULT();
    cfg.over_on_kmh = 0;
    cfg.over_off_kmh = 0;
    ui_model_state_t st;
    ui_model_init(&st, &cfg);
    feed_match(&st, 1000, 40, "X", 1);
    ui_model_t m;

    vehicle_snapshot_t s = snap_of(1000, VS_FRESH, 41.0f, 1);
    ui_model_build(&st, &s, true, &m);
    assert(m.overspeed == true);
}

/* ---------- épocas ---------- */

static void test_match_de_otra_epoca_se_descarta(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    /* El matcher terminó con época 1, pero el estado ya está en época 2. */
    ui_match_input_t m1 = match_of(40, "Lavalle", 1);
    ui_model_on_match(&st, 1000, &m1, 2);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 2);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
    assert(m.show_street == false);
}

static void test_cambio_de_epoca_descarta_limite_mostrado(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);

    /* Cambió el tracker: el límite de la calle del vehículo anterior no se
     * muestra. */
    vehicle_snapshot_t snap = snap_of(1100, VS_FRESH, 30.0f, 2);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
    assert(m.show_street == false);
}

static void test_invalidate(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    feed_match(&st, 1000, 40, "Lavalle", 1);
    ui_model_invalidate(&st);

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_t m;
    ui_model_build(&st, &snap, true, &m);
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
    assert(m.show_street == false);
}

/* ---------- pantallas ---------- */

static void test_secuencia_de_pantallas(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    ui_model_t m;

    /* Arranque: nada llegó todavía. */
    vehicle_snapshot_t s0 = snap_of(100, VS_UNKNOWN, 0.0f, 1);
    ui_model_build(&st, &s0, true, &m);
    assert(m.screen == UI_SCREEN_WAITING_LINK);

    /* Llega un fix: operación normal. */
    vehicle_snapshot_t s1 = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_build(&st, &s1, true, &m);
    assert(m.screen == UI_SCREEN_DRIVING);

    /* Se pierde el fix por completo, pero ya hubo enlace: la falla es del GPS, no
     * «esperando enlace». */
    vehicle_snapshot_t s2 = snap_of(9000, VS_UNKNOWN, 0.0f, 1);
    ui_model_build(&st, &s2, true, &m);
    assert(m.screen == UI_SCREEN_NO_FIX);
}

static void test_ignicion_desconocida(void)
{
    ui_model_state_t st;
    ui_model_init(&st, NULL);
    ui_model_t m;

    vehicle_snapshot_t snap = snap_of(1000, VS_FRESH, 30.0f, 1);
    ui_model_build(&st, &snap, true, &m);
    /* Ignición desconocida se reporta como tal, no como apagada. */
    assert(m.ignition_known == false);
    assert(m.ignition_on == false);

    snap.ignition_state = VS_FRESH;
    snap.ignition_on = true;
    ui_model_build(&st, &snap, true, &m);
    assert(m.ignition_known == true);
    assert(m.ignition_on == true);
}

static void test_punteros_nulos(void)
{
    ui_model_init(NULL, NULL);
    ui_model_invalidate(NULL);
    ui_model_on_match(NULL, 0, NULL, 0);
    ui_model_on_no_match(NULL, 0);
    ui_model_build(NULL, NULL, false, NULL);

    ui_model_state_t st;
    ui_model_init(&st, NULL);
    ui_model_t m;
    ui_model_build(&st, NULL, false, &m);
    assert(m.limit_state == UI_LIMIT_UNKNOWN);
}

static void test_nombres(void)
{
    assert(strcmp(ui_limit_state_name(UI_LIMIT_LAST_KNOWN), "ultimo_conocido") == 0);
    assert(strcmp(ui_speed_state_name(UI_SPEED_LIVE), "en_vivo") == 0);
    assert(strcmp(ui_screen_name(UI_SCREEN_NO_MAPS), "sin_mapas") == 0);
}

int main(void)
{
    test_sin_limite_al_arrancar();
    test_velocidad_visible_antes_del_matching();
    test_velocidad_sin_tarjeta();
    test_velocidad_desconocida_y_vencida();
    test_limite_conocido();
    test_limite_ultimo_conocido();
    test_limite_vencido();
    test_limite_inferido_en_via_anonima();
    test_tramo_sin_limite_no_inventa();
    test_limite_conserva_valor_si_tramo_nuevo_no_declara();
    test_histeresis_enciende_y_apaga();
    test_no_alerta_con_fix_vencido();
    test_no_alerta_con_limite_vencido();
    test_no_alerta_a_paso_de_hombre();
    test_alerta_con_limite_inferido();
    test_umbrales_configurables();
    test_match_de_otra_epoca_se_descarta();
    test_cambio_de_epoca_descarta_limite_mostrado();
    test_invalidate();
    test_secuencia_de_pantallas();
    test_ignicion_desconocida();
    test_punteros_nulos();
    test_nombres();
    printf("ui_model tests passed\n");
    return 0;
}
