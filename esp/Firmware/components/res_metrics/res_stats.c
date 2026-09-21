#include "res_stats.h"

#include <string.h>

const uint32_t res_stat_bucket_edges_us[RES_STAT_BUCKETS] = {
    100, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, 64000, UINT32_MAX,
};

void res_stat_reset(res_stat_t *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->min_us = UINT32_MAX;
}

void res_stat_add(res_stat_t *s, uint32_t sample_us)
{
    if (!s) {
        return;
    }
    if (s->count == 0 && s->min_us == 0) {
        /* Estructura en cero sin reset previo: tratar min como «sin muestras». */
        s->min_us = UINT32_MAX;
    }
    s->count++;
    s->sum_us += sample_us;
    if (sample_us > s->max_us) {
        s->max_us = sample_us;
    }
    if (sample_us < s->min_us) {
        s->min_us = sample_us;
    }
    for (size_t i = 0; i < RES_STAT_BUCKETS; ++i) {
        if (sample_us <= res_stat_bucket_edges_us[i]) {
            s->buckets[i]++;
            return;
        }
    }
    s->buckets[RES_STAT_BUCKETS - 1]++;
}

uint32_t res_stat_mean_us(const res_stat_t *s)
{
    if (!s || s->count == 0) {
        return 0;
    }
    return (uint32_t)(s->sum_us / s->count);
}

uint32_t res_stat_percentile_us(const res_stat_t *s, uint8_t pct)
{
    if (!s || s->count == 0 || pct == 0 || pct > 100) {
        return 0;
    }

    /* Cantidad mínima de muestras que hay que acumular para cubrir el percentil,
     * con techo entero: para pct=95 y count=20 el objetivo es 19. */
    uint64_t target = ((uint64_t)s->count * pct + 99) / 100;
    uint64_t acc = 0;
    for (size_t i = 0; i < RES_STAT_BUCKETS; ++i) {
        acc += s->buckets[i];
        if (acc >= target) {
            return res_stat_bucket_edges_us[i];
        }
    }
    return UINT32_MAX;
}

void res_log_gate_init(res_log_gate_t *g, uint32_t period_ms)
{
    if (!g) {
        return;
    }
    g->period_ms = period_ms;
    g->last_ms = 0;
    g->fired_once = false;
}

bool res_log_gate_due(res_log_gate_t *g, uint32_t now_ms)
{
    if (!g) {
        return false;
    }
    if (!g->fired_once) {
        g->fired_once = true;
        g->last_ms = now_ms;
        return true;
    }
    /* La resta sin signo maneja el desborde de 32 bits sin caso especial. */
    if ((uint32_t)(now_ms - g->last_ms) >= g->period_ms) {
        g->last_ms = now_ms;
        return true;
    }
    return false;
}
