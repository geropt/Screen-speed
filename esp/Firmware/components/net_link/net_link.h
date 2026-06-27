#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// net_link: Capa A — conectividad WiFi station, no-bloqueante.
//
// Las credenciales viven en NVS (namespace "netcfg"), que es la fuente de
// verdad. Se pueden cargar de dos formas:
//   1) Provisioning self-service (componente wifi_portal) -> net_link_set_credentials().
//   2) Pre-carga de fabrica/flota: /sdcard/net_config.json importado a NVS una
//      sola vez con net_link_import_sd_config() (solo si NVS esta vacio).
//
// net_link_connect() levanta WiFi STA leyendo de NVS. Pensado para correr en el
// boot SIN condicionar el arranque: ante cualquier fallo (sin credenciales, sin
// red, timeout) devuelve error y deja WiFi detenido, para que el equipo siga
// operando offline con los tiles que ya tiene.
//
// Este componente NO depende de diag_log (eso vive en main y crearia una
// dependencia circular): loguea por ESP_LOG y expone getters para que app_main
// registre IP/RSSI/heap en diag.log.
// --------------------------------------------------------------------------

// Importa /sdcard/net_config.json a NVS si NVS aun no tiene SSID. No-op si NVS
// ya tiene credenciales o si el archivo no existe / es invalido.
esp_err_t net_link_import_sd_config(void);

// true si hay un SSID guardado en NVS.
bool net_link_have_credentials(void);

// Guarda credenciales en NVS (lo usan el portal de provisioning y la import).
// base_url puede ser NULL/"" (forward-compat con la Capa B).
esp_err_t net_link_set_credentials(const char *ssid, const char *password,
                                   const char *base_url);

// Levanta WiFi STA con las credenciales de NVS y espera IP hasta timeout_ms.
// Devuelve ESP_OK si obtuvo IP; en cualquier otro caso deja WiFi detenido.
esp_err_t net_link_connect(int timeout_ms);

// Asocia la interfaz STA con las credenciales dadas SIN cambiar el modo WiFi ni
// re-inicializar el driver: asume que wifi_portal ya hizo esp_wifi_init()+start()
// en modo APSTA. Es el flujo de provisioning de la Capa B: el SoftAP sigue arriba
// (el celular no se cae) mientras STA obtiene IP para bajar el catalogo en vivo.
// A diferencia de net_link_connect(), en caso de fallo NO hace teardown (deja el
// AP/portal intactos). Tras conectar, net_link_get_base_url()/get_ip() son validos.
esp_err_t net_link_sta_join_keep_ap(const char *ssid, const char *pass, int timeout_ms);

// true si hay asociacion + IP vigente.
bool net_link_is_connected(void);

// RSSI del AP asociado en dBm (0 si no hay conexion).
int net_link_get_rssi(void);

// Copia la IP actual como string ("0.0.0.0" si no hay). buf de >=16 bytes.
void net_link_get_ip(char *buf, size_t len);

// base_url guardado en NVS (cadena vacia si no habia). Forward-compat con la
// Capa B (descarga de tiles); en la Capa A no se usa.
const char *net_link_get_base_url(void);

// Detiene y libera WiFi para recuperar heap cuando no se necesita.
void net_link_stop(void);

#ifdef __cplusplus
}
#endif
