/* Pruebas del núcleo puro de instrumentación (components/res_metrics/res_stats.c).
 *
 * Interesa sobre todo que las cifras no mientan: que el percentil sea una cota
 * superior declarada como tal, que el máximo sea exacto y que el limitador de
 * logs no se cuelgue con el desborde del contador de milisegundos.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "res_stats.h"

static void test_reset_vacio(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    assert(s.count == 0);
    assert(s.sum_us == 0);
    assert(s.max_us == 0);
    assert(res_stat_mean_us(&s) == 0);
    /* Sin muestras no hay percentil que informar. */
    assert(res_stat_percentile_us(&s, 95) == 0);
}

static void test_una_muestra(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    res_stat_add(&s, 350);
    assert(s.count == 1);
    assert(s.max_us == 350);
    assert(s.min_us == 350);
    assert(res_stat_mean_us(&s) == 350);
    /* 350 µs cae en la cubeta cuyo borde superior es 500. */
    assert(res_stat_percentile_us(&s, 95) == 500);
    assert(res_stat_percentile_us(&s, 50) == 500);
}

static void test_media_y_maximo(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    res_stat_add(&s, 100);
    res_stat_add(&s, 200);
    res_stat_add(&s, 300);
    assert(res_stat_mean_us(&s) == 200);
    assert(s.max_us == 300);
    assert(s.min_us == 100);
}

static void test_percentil_es_cota_superior(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    /* 95 muestras rápidas y 5 lentas: el p95 debe quedar por debajo del máximo. */
    for (int i = 0; i < 95; ++i) {
        res_stat_add(&s, 90);       /* cubeta <=100 */
    }
    for (int i = 0; i < 5; ++i) {
        res_stat_add(&s, 30000);    /* cubeta <=32000 */
    }
    assert(s.count == 100);
    assert(res_stat_percentile_us(&s, 95) == 100);
    assert(res_stat_percentile_us(&s, 99) == 32000);
    /* El máximo sí es exacto: es el dato que hay que citar cuando el percentil
     * sólo da una cota. */
    assert(s.max_us == 30000);
}

static void test_ultima_cubeta_no_inventa_numero(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    res_stat_add(&s, 500000);   /* medio segundo: por encima de todos los bordes */
    /* No se devuelve un borde inventado: la última cubeta informa UINT32_MAX y
     * el llamador debe mirar max_us. */
    assert(res_stat_percentile_us(&s, 95) == UINT32_MAX);
    assert(s.max_us == 500000);
    assert(s.buckets[RES_STAT_BUCKETS - 1] == 1);
}

static void test_bordes_exactos_pertenecen_a_su_cubeta(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    res_stat_add(&s, 100);
    assert(s.buckets[0] == 1);
    res_stat_reset(&s);
    res_stat_add(&s, 101);
    assert(s.buckets[0] == 0);
    assert(s.buckets[1] == 1);
}

static void test_percentiles_invalidos(void)
{
    res_stat_t s;
    res_stat_reset(&s);
    res_stat_add(&s, 100);
    assert(res_stat_percentile_us(&s, 0) == 0);
    assert(res_stat_percentile_us(&s, 101) == 0);
    assert(res_stat_percentile_us(NULL, 95) == 0);
}

static void test_estructura_en_cero_sin_reset(void)
{
    /* Estado estático recién arrancado: todo en cero, sin reset explícito.
     * min_us debe terminar valiendo la muestra, no 0. */
    res_stat_t s;
    memset(&s, 0, sizeof(s));
    res_stat_add(&s, 700);
    assert(s.count == 1);
    assert(s.min_us == 700);
    assert(s.max_us == 700);
}

static void test_gate_primera_vez_y_periodo(void)
{
    res_log_gate_t g;
    res_log_gate_init(&g, 1000);
    /* La primera llamada habilita, para dejar una línea al arrancar. */
    assert(res_log_gate_due(&g, 5000) == true);
    assert(res_log_gate_due(&g, 5001) == false);
    assert(res_log_gate_due(&g, 5999) == false);
    assert(res_log_gate_due(&g, 6000) == true);
    assert(res_log_gate_due(&g, 6001) == false);
}

static void test_gate_sobrevive_desborde(void)
{
    res_log_gate_t g;
    res_log_gate_init(&g, 1000);
    assert(res_log_gate_due(&g, 0xFFFFFF00u) == true);
    assert(res_log_gate_due(&g, 0xFFFFFF10u) == false);
    /* El contador dio la vuelta: 0x100 - 0xFFFFFF00 = 512 ms transcurridos. */
    assert(res_log_gate_due(&g, 0x00000100u) == false);
    /* 0x400 - 0xFFFFFF00 = 1280 ms: corresponde imprimir. */
    assert(res_log_gate_due(&g, 0x00000400u) == true);
}

static void test_gate_nulo(void)
{
    assert(res_log_gate_due(NULL, 0) == false);
    res_log_gate_init(NULL, 100); /* no debe romper */
}

int main(void)
{
    test_reset_vacio();
    test_una_muestra();
    test_media_y_maximo();
    test_percentil_es_cota_superior();
    test_ultima_cubeta_no_inventa_numero();
    test_bordes_exactos_pertenecen_a_su_cubeta();
    test_percentiles_invalidos();
    test_estructura_en_cero_sin_reset();
    test_gate_primera_vez_y_periodo();
    test_gate_sobrevive_desborde();
    test_gate_nulo();
    printf("res_stats tests passed\n");
    return 0;
}
