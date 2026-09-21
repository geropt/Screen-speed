/* Instrumentación de recursos del HUD (P01).
 *
 * Responde a la pregunta que el plan pone antes de integrar red, mapas o app:
 * cuánto margen hay realmente. Mide, no adivina, y deja el resultado en un log
 * acotado y en una estructura consultable.
 *
 * Lo que registra:
 *  - heap interno, capaz de DMA y PSRAM: libre, mayor bloque y mínimo histórico;
 *  - marca de agua de stack de las tareas registradas;
 *  - latencias por canal (flush del panel, drenaje de UART, matching…) con
 *    máximo exacto y cotas superiores de percentil;
 *  - contadores de error por categoría.
 *
 * Lo que NO hace: no reserva memoria propia, no crea tareas, no imprime por
 * evento. Todo el estado es estático y acotado, para que instrumentar no cambie
 * lo que se está midiendo. Nada se marca IRAM_ATTR: en la línea base P00 la IRAM
 * está a 1 byte del límite.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "res_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Canales de latencia. Fijos y pocos: cada uno cuesta una `res_stat_t`. */
typedef enum {
    RES_CH_LCD_FLUSH = 0,   /**< de entregar el buffer al panel hasta DMA lista */
    RES_CH_LVGL_TICK,       /**< duración de lv_timer_handler() + ui_tick()     */
    RES_CH_UART_DRAIN,      /**< intervalo entre drenajes de la UART            */
    RES_CH_MAP_MATCH,       /**< duración de una consulta de mapa               */
    RES_CH_FIX_TO_DISPLAY,  /**< de fix válido a flush completado               */
    RES_CH_COUNT
} res_channel_t;

/** Categorías de error. Contadores, no logs por evento. */
typedef enum {
    RES_ERR_UART_OVERFLOW = 0,
    RES_ERR_UART_FRAMING,
    RES_ERR_QUEUE_FULL,
    RES_ERR_ALLOC_FAILED,
    RES_ERR_SD_IO,
    RES_ERR_I2C_BUS,
    RES_ERR_COUNT
} res_error_t;

typedef struct {
    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t internal_min_ever;
    uint32_t dma_free;
    uint32_t dma_largest;
    uint32_t psram_free;
    uint32_t psram_largest;
    uint32_t psram_min_ever;
    uint32_t uptime_ms;
} res_mem_snapshot_t;

/** Inicializa el estado. Idempotente; puede llamarse antes que cualquier tarea. */
void res_metrics_init(void);

/** Lee el estado de memoria en este instante. */
void res_metrics_mem(res_mem_snapshot_t *out);

/**
 * @brief Marca el comienzo de una medición en un canal.
 *
 * Un solo escritor por canal. `begin` puede ocurrir en una tarea y `end` en el
 * callback de DMA correspondiente (así se mide RES_CH_LCD_FLUSH), pero dos
 * mediciones simultáneas en el mismo canal se pisan: la segunda gana.
 */
void res_metrics_begin(res_channel_t ch);

/** Cierra la medición abierta y suma la muestra. Sin `begin` previo, no hace nada. */
void res_metrics_end(res_channel_t ch);

/** Suma una muestra ya medida por el llamador. */
void res_metrics_sample_us(res_channel_t ch, uint32_t us);

/**
 * @brief Registra el intervalo entre llamadas consecutivas en un canal.
 *
 * Para métricas de cadencia como el drenaje de UART, donde interesa el hueco
 * entre eventos y no la duración de cada uno. La primera llamada sólo fija el
 * origen.
 */
void res_metrics_mark_interval(res_channel_t ch);

/** Copia la estadística de un canal. */
void res_metrics_get(res_channel_t ch, res_stat_t *out);

/** Incrementa un contador de error. */
void res_metrics_error(res_error_t kind);

/** @return cantidad acumulada de errores de una categoría. */
uint32_t res_metrics_error_count(res_error_t kind);

/**
 * @brief Registra una tarea para vigilar su marca de agua de stack.
 *
 * @param name  etiqueta corta para el log; no se copia, debe ser estática
 * @param task  handle de la tarea, o NULL para la tarea actual
 * @return true si entró en la tabla (capacidad acotada)
 */
bool res_metrics_watch_task(const char *name, void *task);

/**
 * @brief Imprime una vez por período el estado de recursos.
 *
 * Llamar desde una tarea que ya corre periódicamente; no crea tareas propias.
 * El período sale de CONFIG_RES_METRICS_LOG_PERIOD_MS. Si la instrumentación
 * está deshabilitada en configuración, no hace nada.
 */
void res_metrics_log_due(void);

/** Fuerza el volcado completo, sin esperar el período. */
void res_metrics_log_now(void);

#ifdef __cplusplus
}
#endif
