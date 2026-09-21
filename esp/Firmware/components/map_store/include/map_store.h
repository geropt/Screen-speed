/* Almacenamiento de mapas: dueño de la caché, de la generación y del ciclo de vida.
 *
 * Separado del matcher en P04. El matcher hace geometría y no sabe de archivos; este
 * componente sabe de archivos y no sabe de geometría.
 *
 * Qué define, que antes no estaba definido en ningún lado:
 *
 *  - **Ownership de los bytes de un tile.** `map_store_fetch()` devuelve un puntero
 *    a memoria de la caché. Es válido hasta la próxima llamada a `fetch` o hasta un
 *    cambio de generación. El consumidor NO lo libera y NO lo guarda entre fixes.
 *  - **Generación.** Cada montaje, remount o instalación de mapas incrementa la
 *    generación. Un resultado calculado con una generación vieja se puede rechazar
 *    comparando el número, sin depender de tiempos. Es el mismo mecanismo que
 *    `tracker_epoch` en el estado del vehículo, aplicado al almacenamiento.
 *  - **Disponibilidad.** Sin mapas montados, `fetch` devuelve NULL siempre y no
 *    intenta abrir nada: el velocímetro sigue funcionando y no se gastan nueve
 *    aperturas fallidas por fix.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "map_match.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t generation;
    uint32_t fetches;
    uint32_t absent;
    uint32_t io_errors;
} map_store_stats_t;

/** Qué backend resuelve las celdas. */
typedef enum {
    MAP_BACKEND_DIRECTORY = 0,  /**< un archivo por celda en la tarjeta   */
    MAP_BACKEND_CAPSULE         /**< una cápsula consultada por rangos    */
} map_backend_t;

/** Inicializa. Idempotente. Arranca sin mapas disponibles. */
void map_store_init(void);

/**
 * @brief Elige el backend de lectura.
 *
 * Los dos entregan **los mismos bytes** para la misma celda: la equivalencia está
 * probada en host contra el golden de P00. Cambiar de backend incrementa la
 * generación, porque el contenido puede no ser el mismo mapa.
 *
 * @param capsule_path ruta de la cápsula cuando el backend es MAP_BACKEND_CAPSULE
 */
esp_err_t map_store_set_backend(map_backend_t backend, const char *capsule_path);

/** @return backend en uso. */
map_backend_t map_store_backend(void);

/**
 * @brief Declara que hay mapas usables e incrementa la generación.
 *
 * La llama quien monta la tarjeta. Invalida la caché, porque el contenido puede ser
 * de otra tarjeta.
 */
void map_store_set_available(bool available);

/** @return true si hay mapas usables ahora. */
bool map_store_is_available(void);

/** @return generación actual; 0 significa «nunca hubo mapas». */
uint64_t map_store_generation(void);

/**
 * @brief Fuente de candidatos para el matcher, con la generación actual embebida.
 *
 * Tomarla justo antes de consultar: la generación viaja al resultado.
 */
map_tile_source_t map_store_tile_source(void);

/** Implementación de `map_tile_fetch_fn`. Ver ownership en el encabezado. */
const uint8_t *map_store_fetch(void *ctx, int32_t origin_lat_e7,
                              int32_t origin_lon_e7, size_t *out_len);

void map_store_get_stats(map_store_stats_t *out);

#ifdef __cplusplus
}
#endif
