#include "res_metrics.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "RES";

#define WATCHED_TASKS_MAX 8

typedef struct {
    const char *name;
    TaskHandle_t task;
    uint32_t     min_watermark;
} watched_task_t;

static struct {
    bool           inited;
    res_stat_t     ch[RES_CH_COUNT];
    int64_t        open_us[RES_CH_COUNT];
    int64_t        last_mark_us[RES_CH_COUNT];
    uint32_t       err[RES_ERR_COUNT];
    watched_task_t tasks[WATCHED_TASKS_MAX];
    uint32_t       task_count;
    uint32_t       psram_min_ever;
    res_log_gate_t gate;
} s;

static const char *ch_name(res_channel_t ch)
{
    switch (ch) {
    case RES_CH_LCD_FLUSH:      return "lcd_flush";
    case RES_CH_LVGL_TICK:      return "lvgl_tick";
    case RES_CH_UART_DRAIN:     return "uart_drain";
    case RES_CH_MAP_MATCH:      return "map_match";
    case RES_CH_FIX_TO_DISPLAY: return "fix2disp";
    default:                    return "?";
    }
}

static const char *err_name(res_error_t e)
{
    switch (e) {
    case RES_ERR_UART_OVERFLOW: return "uart_ovf";
    case RES_ERR_UART_FRAMING:  return "uart_frame";
    case RES_ERR_QUEUE_FULL:    return "queue_full";
    case RES_ERR_ALLOC_FAILED:  return "alloc_fail";
    case RES_ERR_SD_IO:         return "sd_io";
    case RES_ERR_I2C_BUS:       return "i2c_bus";
    default:                    return "?";
    }
}

void res_metrics_init(void)
{
    if (s.inited) {
        return;
    }
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < RES_CH_COUNT; ++i) {
        res_stat_reset(&s.ch[i]);
    }
    s.psram_min_ever = UINT32_MAX;
    res_log_gate_init(&s.gate, CONFIG_RES_METRICS_LOG_PERIOD_MS);
    s.inited = true;
}

void res_metrics_mem(res_mem_snapshot_t *out)
{
    if (!out) {
        return;
    }
    out->internal_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_largest  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->internal_min_ever = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->dma_free          = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    out->dma_largest       = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
#if CONFIG_SPIRAM
    out->psram_free        = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_largest     = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->psram_min_ever    = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
#else
    out->psram_free = out->psram_largest = out->psram_min_ever = 0;
#endif
    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void res_metrics_begin(res_channel_t ch)
{
    if (!s.inited || ch >= RES_CH_COUNT) {
        return;
    }
    s.open_us[ch] = esp_timer_get_time();
}

void res_metrics_end(res_channel_t ch)
{
    if (!s.inited || ch >= RES_CH_COUNT || s.open_us[ch] == 0) {
        return;
    }
    int64_t delta = esp_timer_get_time() - s.open_us[ch];
    s.open_us[ch] = 0;
    if (delta < 0) {
        return;
    }
    res_stat_add(&s.ch[ch], (uint32_t)delta);
}

void res_metrics_sample_us(res_channel_t ch, uint32_t us)
{
    if (!s.inited || ch >= RES_CH_COUNT) {
        return;
    }
    res_stat_add(&s.ch[ch], us);
}

void res_metrics_mark_interval(res_channel_t ch)
{
    if (!s.inited || ch >= RES_CH_COUNT) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s.last_mark_us[ch] != 0) {
        int64_t delta = now - s.last_mark_us[ch];
        if (delta >= 0) {
            res_stat_add(&s.ch[ch], (uint32_t)delta);
        }
    }
    s.last_mark_us[ch] = now;
}

void res_metrics_get(res_channel_t ch, res_stat_t *out)
{
    if (!out) {
        return;
    }
    if (!s.inited || ch >= RES_CH_COUNT) {
        res_stat_reset(out);
        return;
    }
    *out = s.ch[ch];
}

void res_metrics_error(res_error_t kind)
{
    if (!s.inited || kind >= RES_ERR_COUNT) {
        return;
    }
    s.err[kind]++;
}

uint32_t res_metrics_error_count(res_error_t kind)
{
    if (!s.inited || kind >= RES_ERR_COUNT) {
        return 0;
    }
    return s.err[kind];
}

bool res_metrics_watch_task(const char *name, void *task)
{
    if (!s.inited || s.task_count >= WATCHED_TASKS_MAX || !name) {
        return false;
    }
    s.tasks[s.task_count].name = name;
    s.tasks[s.task_count].task = task ? (TaskHandle_t)task : xTaskGetCurrentTaskHandle();
    s.tasks[s.task_count].min_watermark = UINT32_MAX;
    s.task_count++;
    return true;
}

static void refresh_watermarks(void)
{
    for (uint32_t i = 0; i < s.task_count; ++i) {
        if (!s.tasks[i].task) {
            continue;
        }
        /* En bytes: uxTaskGetStackHighWaterMark devuelve palabras en algunos
         * puertos, pero en ESP-IDF (Xtensa) ya devuelve bytes. */
        uint32_t wm = (uint32_t)uxTaskGetStackHighWaterMark(s.tasks[i].task);
        if (wm < s.tasks[i].min_watermark) {
            s.tasks[i].min_watermark = wm;
        }
    }
}

void res_metrics_log_now(void)
{
#if CONFIG_RES_METRICS_ENABLE
    if (!s.inited) {
        return;
    }

    res_mem_snapshot_t m;
    res_metrics_mem(&m);
    if (m.psram_min_ever < s.psram_min_ever) {
        s.psram_min_ever = m.psram_min_ever;
    }

    ESP_LOGI(TAG, "mem interna libre=%" PRIu32 " mayor=%" PRIu32 " min=%" PRIu32
                  " | dma libre=%" PRIu32 " mayor=%" PRIu32
                  " | psram libre=%" PRIu32 " mayor=%" PRIu32 " min=%" PRIu32,
             m.internal_free, m.internal_largest, m.internal_min_ever,
             m.dma_free, m.dma_largest,
             m.psram_free, m.psram_largest, m.psram_min_ever);

    refresh_watermarks();
    for (uint32_t i = 0; i < s.task_count; ++i) {
        ESP_LOGI(TAG, "stack %s: margen actual=%" PRIu32 " B, mínimo visto=%" PRIu32 " B",
                 s.tasks[i].name,
                 (uint32_t)uxTaskGetStackHighWaterMark(s.tasks[i].task),
                 s.tasks[i].min_watermark);
    }

    for (int i = 0; i < RES_CH_COUNT; ++i) {
        const res_stat_t *st = &s.ch[i];
        if (st->count == 0) {
            continue;
        }
        uint32_t p95 = res_stat_percentile_us(st, 95);
        /* p95 es cota superior de cubeta; UINT32_MAX significa «cayó en la
         * última cubeta», y entonces sólo el máximo es informativo. */
        if (p95 == UINT32_MAX) {
            ESP_LOGI(TAG, "%s: n=%" PRIu32 " media=%" PRIu32 "us p95=>64000us max=%" PRIu32 "us",
                     ch_name((res_channel_t)i), st->count, res_stat_mean_us(st), st->max_us);
        } else {
            ESP_LOGI(TAG, "%s: n=%" PRIu32 " media=%" PRIu32 "us p95<=%" PRIu32 "us max=%" PRIu32 "us",
                     ch_name((res_channel_t)i), st->count, res_stat_mean_us(st), p95, st->max_us);
        }
    }

    bool any_err = false;
    for (int i = 0; i < RES_ERR_COUNT; ++i) {
        if (s.err[i]) {
            any_err = true;
            ESP_LOGW(TAG, "errores %s: %" PRIu32, err_name((res_error_t)i), s.err[i]);
        }
    }
    if (!any_err) {
        ESP_LOGI(TAG, "sin errores registrados");
    }
#endif
}

void res_metrics_log_due(void)
{
#if CONFIG_RES_METRICS_ENABLE
    if (!s.inited) {
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (res_log_gate_due(&s.gate, now_ms)) {
        res_metrics_log_now();
    }
#endif
}
