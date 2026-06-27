#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// wifi_portal: provisioning self-service via SoftAP + portal cautivo.
//
// Levanta un AP "Mykeego-XXXX", sirve una pagina web (lista de redes WiFi
// cercanas + form de password) y hace DNS hijack para que el celular abra el
// portal solo. Cuando el cliente guarda una red, las credenciales se persisten
// en NVS (via net_link_set_credentials) y la funcion retorna ESP_OK; el llamador
// deberia reiniciar el equipo para conectar como STA con las nuevas credenciales.
//
// Flujo transitorio y BLOQUEANTE: corre hasta que se guarda una red o vence
// timeout_ms. Antes de llamarla conviene net_link_stop() para partir de un WiFi
// limpio.
// --------------------------------------------------------------------------

// Devuelve ESP_OK si se guardo una red (reiniciar despues), ESP_ERR_TIMEOUT si
// nadie configuro, u otro error en el bring-up del AP.
esp_err_t wifi_portal_run(int timeout_ms);

// Calcula el SSID del AP ("Mykeego-XXXX", sufijo del MAC) sin levantar el AP.
// Util para que la UI arme el QR de union a la red antes/durante el portal.
void wifi_portal_get_ap_ssid(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
