#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// map_sync: Capa B — distribucion de tiles de mapas por zona.
//
// Un servicio HTTPS expone:
//   GET <base_url>/catalog.json        -> catalogo de zonas
//   GET <base_url>/zones/<id>.json     -> manifiesto (tiles con sha256/size) de una zona
//   GET <base_url>/<tile>.bin          -> el tile (url relativa viene en el manifiesto)
//
// base_url vive en NVS (namespace "netcfg", via net_link). Las zonas elegidas y
// el estado de versiones viven en NVS namespace "mapcfg".
//
// El WiFi es on-demand: map_sync NO levanta ni baja WiFi por su cuenta (salvo el
// selftest). El llamador es dueno del lifecycle (net_link_connect/stop).
//
// Verificacion TLS SIEMPRE activa: produccion usa el certificate bundle; para el
// servidor de prueba self-signed, activar CONFIG_MAP_SYNC_TEST_CERT y embeber el
// PEM (ver components/map_sync/Kconfig).
// --------------------------------------------------------------------------

typedef struct {
    char   id[24];     // id de zona (ej. "caba_centro")
    char   name[40];   // nombre legible
    int    version;    // version del contenido de la zona
    int    tiles;      // cantidad de tiles
    size_t bytes;      // tamano total de la zona
} map_zone_t;

// Baja y parsea <base_url>/catalog.json. Asume WiFi STA ya conectado.
// En exito devuelve ESP_OK y un array recien asignado en *out (liberar con
// map_sync_free_catalog) y la cantidad en *count.
esp_err_t map_sync_fetch_catalog(map_zone_t **out, int *count);

// Libera el array devuelto por map_sync_fetch_catalog.
void map_sync_free_catalog(map_zone_t *zones);

// --- Zonas elegidas por el cliente (NVS namespace "mapcfg", clave "sel_zones") ---

// Persiste la lista de ids de zonas elegidas (CSV en NVS). n=0 borra la seleccion.
esp_err_t map_sync_set_selected(const char *const *ids, int n);

// Lee los ids elegidos en ids[][24] (hasta max). Devuelve la cantidad leida.
int map_sync_get_selected(char ids[][24], int max);

// --- Sincronizacion (delta + descarga) ---

typedef struct {
    int  checked;      // tiles inspeccionados
    int  changed;      // tiles que requerian descarga (faltantes o size distinto)
    int  downloaded;   // tiles descargados y verificados OK
    int  failed;       // descargas/verificaciones fallidas
    bool any_update;   // true si bajo >=1 tile (se levanto el flag "actualizado")
} map_sync_result_t;

// Para cada zona elegida: baja su manifiesto, calcula el delta contra los tiles
// locales (size; sha256 del tile descargado siempre se verifica) y descarga los
// que cambiaron a "<archivo>.tmp" -> rename atomico. Asume WiFi ya conectado; NO
// maneja el lifecycle de WiFi. Setea el flag "mapas actualizados" si bajo algo.
esp_err_t map_sync_run(map_sync_result_t *out);

// Flag persistente "hay mapas nuevos" (NVS), para mostrar al boot.
bool map_sync_updates_pending(void);
void map_sync_clear_updates_flag(void);

// Selftest (gated por CONFIG_MAP_SYNC_BOOT_SELFTEST): conecta WiFi STA, baja el
// catalogo, loguea las zonas y desconecta. Pensado para validar HTTPS + cert +
// parseo en el device sin tocar el portal. No-op si el flag esta apagado.
void map_sync_selftest(void);

#ifdef __cplusplus
}
#endif
