/* Cápsula de mapas: un solo archivo consultable por rangos.
 *
 * Objetivo, del plan: **empaquetar los payloads actuales sin cambiar su binario**. La
 * cápsula no reemplaza el formato v1 de los tiles; los contiene tal cual. El matcher
 * recibe exactamente los mismos bytes que recibía del directorio, y eso se prueba
 * comparando contra el golden congelado en P00.
 *
 * Por qué un contenedor. Con 1 961 archivos, actualizar cobertura significa escribir,
 * verificar y activar casi dos mil entradas de FAT, sin ninguna transición atómica
 * disponible. Un archivo único se recibe, se verifica completo y se activa cambiando
 * un selector: la unidad de fallo pasa a ser «la cápsula entera», que sí se puede
 * razonar.
 *
 * Estructura:
 *
 *   [cabecera 64 B][manifiesto][índice ordenado][región de payloads v1]
 *
 * El acceso es por rangos: `read_at(offset, len)`. No hay rutas por celda, no hay
 * nombres de archivo. Buscar una celda es una búsqueda binaria sobre el índice.
 *
 * INTEGRIDAD, NO AUTENTICIDAD. Los CRC-32 de esta implementación detectan corrupción
 * —escritura interrumpida, tarjeta dañada, transferencia truncada— y **no** detectan
 * manipulación: cualquiera que edite el contenido puede recalcular el CRC. El campo
 * de firma del manifiesto existe y hoy está en cero: verificarla contra una clave es
 * trabajo de P10. Este componente no debe presentarse como control de autenticidad.
 *
 * Sin ESP-IDF, sin memoria dinámica: el lector se inyecta. Se prueba en el host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPSULE_MAGIC          "MKGOCAPS"
#define CAPSULE_MAGIC_LEN      8
#define CAPSULE_HEADER_SIZE    64
/* Versión del contenedor. Independiente de la versión geográfica del contenido y de
 * la del firmware, como pide la propuesta. */
#define CAPSULE_CONTAINER_VER  1
/* Versión mínima de lector que la cápsula exige. Un lector más viejo debe rechazarla
 * en lugar de interpretarla mal. */
#define CAPSULE_READER_VER     1

#define CAPSULE_REGION_MAX     64      /* nombre de región, bytes            */
#define CAPSULE_SIG_MAX        64      /* reservado para P10                 */

/* Entrada del índice: 24 bytes.
 *
 * La propuesta estimaba 56 B por entrada; esta implementación usa 24 porque no
 * guarda un hash completo por payload —el CRC-32 alcanza para integridad— y no
 * necesita los campos que la estimación reservaba. Con 1 961 celdas el índice mide
 * 47 064 B, así que se puede leer por páginas sin cargarlo entero.
 */
#define CAPSULE_INDEX_ENTRY_SIZE 24

typedef struct {
    int32_t  origin_lat_e7;
    int32_t  origin_lon_e7;
    uint64_t payload_offset;   /**< absoluto en el archivo */
    uint32_t payload_len;
    uint32_t payload_crc32;
} capsule_index_entry_t;

/** Manifiesto: identidad y compatibilidad. Serializado a continuación de la cabecera. */
typedef struct {
    char     region[CAPSULE_REGION_MAX];  /**< nombre legible de la cobertura   */
    uint32_t content_version;             /**< versión geográfica del contenido */
    uint32_t tile_size_e7;                /**< parámetros geométricos del v1    */
    uint32_t filename_scale;
    uint32_t cell_count;
    uint64_t created_unix;
    uint32_t index_crc32;                 /**< CRC del índice completo          */
    uint8_t  signature[CAPSULE_SIG_MAX];  /**< cero hasta P10                   */
} capsule_manifest_t;

/**
 * @brief Lectura por rangos del contenedor.
 *
 * @param ctx    contexto opaco
 * @param offset desplazamiento absoluto
 * @param out    destino
 * @param len    bytes a leer
 * @return bytes leídos; menos que `len` significa fin de archivo o error
 */
typedef size_t (*capsule_read_at_fn)(void *ctx, uint64_t offset, uint8_t *out, size_t len);

typedef enum {
    CAPSULE_OK = 0,
    CAPSULE_ERR_IO,             /**< la lectura devolvió menos de lo pedido      */
    CAPSULE_ERR_MAGIC,          /**< no es una cápsula                           */
    CAPSULE_ERR_VERSION,        /**< el lector es más viejo que lo que se exige   */
    CAPSULE_ERR_HEADER,         /**< cabecera inconsistente                       */
    CAPSULE_ERR_INDEX_CRC,      /**< el índice no verifica                        */
    CAPSULE_ERR_GEOMETRY,       /**< parámetros geométricos que el lector no usa  */
    CAPSULE_ERR_TRUNCATED,      /**< el archivo es más corto que lo que declara   */
    CAPSULE_ERR_PAYLOAD_CRC,    /**< un payload no verifica                       */
    CAPSULE_ERR_NOT_FOUND       /**< la celda no está en la cobertura             */
} capsule_err_t;

typedef struct {
    capsule_read_at_fn read_at;
    void              *ctx;
    uint64_t           file_size;

    /* Rellenado por `map_capsule_open`. */
    capsule_manifest_t manifest;
    uint64_t           index_offset;
    uint32_t           index_count;
    uint64_t           payload_region_offset;
    uint64_t           payload_region_len;
    bool               open;

    /* Contadores de diagnóstico. */
    uint32_t           lookups;
    uint32_t           not_found;
    uint32_t           crc_failures;
} map_capsule_t;

/**
 * @brief Abre y valida la cápsula: cabecera, versión, manifiesto e índice.
 *
 * Verifica el CRC del índice completo. NO verifica los payloads: eso se hace al
 * leerlos, o con `map_capsule_verify_all()` antes de activar.
 */
capsule_err_t map_capsule_open(map_capsule_t *cap, capsule_read_at_fn read_at,
                              void *ctx, uint64_t file_size);

/**
 * @brief Busca una celda por su origen.
 *
 * Búsqueda binaria sobre el índice ordenado por (lat, lon). Lee sólo las páginas del
 * índice que necesita.
 */
capsule_err_t map_capsule_find(map_capsule_t *cap, int32_t origin_lat_e7,
                              int32_t origin_lon_e7, capsule_index_entry_t *out);

/**
 * @brief Lee el payload de una entrada y verifica su CRC.
 *
 * @param out    destino; debe tener al menos `entry->payload_len` bytes
 * @param out_cap capacidad del destino
 */
capsule_err_t map_capsule_read_payload(map_capsule_t *cap,
                                      const capsule_index_entry_t *entry,
                                      uint8_t *out, size_t out_cap);

/**
 * @brief Verifica TODOS los payloads y el índice.
 *
 * Es lo que hay que correr antes de marcar una candidata como lista: el plan exige
 * «verificar completamente y reabrir con lector real antes de marcar READY».
 *
 * @param out_checked cantidad de celdas verificadas, opcional
 */
capsule_err_t map_capsule_verify_all(map_capsule_t *cap, uint32_t *out_checked);

/** CRC-32 (polinomio reflejado 0xEDB88320), expuesto para el empaquetador y pruebas. */
uint32_t capsule_crc32(const uint8_t *data, size_t len);
uint32_t capsule_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

const char *capsule_err_name(capsule_err_t e);

#ifdef __cplusplus
}
#endif
