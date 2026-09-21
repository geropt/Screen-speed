/* Núcleo puro de estadística para la instrumentación de recursos.
 *
 * Sin dependencias de ESP-IDF a propósito: así el cálculo de percentiles y el
 * limitador de logs se prueban en el host (host_tests/test_res_stats.c) y en el
 * dispositivo queda sólo la lectura de heap, stacks y relojes.
 *
 * Criterio de honestidad de las cifras: los percentiles se calculan sobre un
 * histograma de cubetas fijas, así que lo que se devuelve es el **borde superior
 * de la cubeta** donde cae el percentil, es decir una cota superior. Un p95 de
 * 2 000 µs significa «el 95 % de las muestras estuvo por debajo de 2 000 µs», no
 * «el p95 es 2 000 µs». El máximo sí es exacto porque se guarda aparte.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bordes superiores de cubeta en microsegundos. Cubren desde un flush corto
 * (cientos de µs) hasta un bloqueo largo (decenas de ms), que es el rango que
 * interesa para las metas del plan: drenaje de UART <25 ms y fix→display
 * <200 ms. La última cubeta es «todo lo demás». */
#define RES_STAT_BUCKETS 11

extern const uint32_t res_stat_bucket_edges_us[RES_STAT_BUCKETS];

typedef struct {
    uint32_t count;
    uint64_t sum_us;
    uint32_t max_us;
    uint32_t min_us;
    uint32_t buckets[RES_STAT_BUCKETS];
} res_stat_t;

/** Deja la estadística en cero. */
void res_stat_reset(res_stat_t *s);

/** Suma una muestra. */
void res_stat_add(res_stat_t *s, uint32_t sample_us);

/** @return media en µs, o 0 sin muestras. */
uint32_t res_stat_mean_us(const res_stat_t *s);

/**
 * @brief Cota superior del percentil pedido.
 *
 * @param pct percentil en 1..100
 * @return borde superior de la cubeta donde cae el percentil. Si el percentil
 *         cae en la última cubeta, devuelve UINT32_MAX para no inventar un
 *         número: en ese caso hay que mirar `max_us`.
 */
uint32_t res_stat_percentile_us(const res_stat_t *s, uint8_t pct);

/* ---------- Limitador de logs ---------- */

/**
 * @brief Compuerta para que un log periódico no inunde la consola.
 *
 * El plan pide logs acotados y explícitamente «sin tráfico por píxel»: la
 * instrumentación cuenta siempre, pero imprime como máximo una vez por período.
 */
typedef struct {
    uint32_t period_ms;
    uint32_t last_ms;
    bool     fired_once;
} res_log_gate_t;

/** Inicializa la compuerta con su período. */
void res_log_gate_init(res_log_gate_t *g, uint32_t period_ms);

/**
 * @brief ¿Corresponde imprimir ahora?
 *
 * La primera llamada siempre habilita, para que un arranque deje una línea.
 * Tolera el desborde del contador de milisegundos.
 *
 * @param now_ms reloj monotónico en ms
 */
bool res_log_gate_due(res_log_gate_t *g, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
