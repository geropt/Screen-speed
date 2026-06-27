#include "net_link.h"
#include "sd_manager.h"   // MOUNT_POINT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "NET_LINK";

#define NET_CONFIG_PATH   MOUNT_POINT "/net_config.json"

// NVS: store de credenciales (fuente de verdad).
#define NVS_NS        "netcfg"
#define NVS_K_SSID    "ssid"
#define NVS_K_PASS    "password"
#define NVS_K_URL     "base_url"

// Reintentos de asociacion antes de rendirse (acotado por timeout_ms igual).
#define NET_LINK_MAX_RETRY   5

// Bits del event group.
#define NL_CONNECTED_BIT   BIT0
#define NL_FAIL_BIT        BIT1

// ---- estado del modulo ----
static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t       *s_netif       = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler   = NULL;
static bool s_started   = false;   // esp_wifi_start() en curso
static bool s_connected = false;   // asociado + IP
static int  s_retry     = 0;

static char s_ip[16]   = "0.0.0.0";
static char s_base_url[128] = "";

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// Lee una clave string de NVS:netcfg. Devuelve false si falta o esta vacia.
static bool nvs_get_str_field(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return (err == ESP_OK && out[0] != '\0');
}

esp_err_t net_link_set_credentials(const char *ssid, const char *password,
                                   const char *base_url)
{
    if (ssid == NULL || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = ensure_nvs();
    if (err != ESP_OK)
        return err;

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = nvs_set_str(h, NVS_K_SSID, ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, NVS_K_PASS, password ? password : "");
    if (err == ESP_OK)
        err = nvs_set_str(h, NVS_K_URL, base_url ? base_url : "");
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "credenciales guardadas en NVS (ssid='%s')", ssid);
    return err;
}

bool net_link_have_credentials(void)
{
    if (ensure_nvs() != ESP_OK)
        return false;
    char ssid[33];
    return nvs_get_str_field(NVS_K_SSID, ssid, sizeof(ssid));
}

// ---------------------------------------------------------------------------
// Pre-carga: importar /sdcard/net_config.json -> NVS (solo si NVS vacio)
// ---------------------------------------------------------------------------
esp_err_t net_link_import_sd_config(void)
{
    if (net_link_have_credentials()) {
        ESP_LOGI(TAG, "NVS ya tiene credenciales; no se importa la SD");
        return ESP_OK;
    }

    FILE *f = fopen(NET_CONFIG_PATH, "rb");
    if (f == NULL)
        return ESP_ERR_NOT_FOUND;   // sin pre-carga: provisioning a demanda

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) {
        ESP_LOGW(TAG, "%s tamano invalido (%ld)", NET_CONFIG_PATH, sz);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(sz + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s JSON invalido", NET_CONFIG_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *j_url  = cJSON_GetObjectItemCaseSensitive(root, "base_url");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(j_ssid) && j_ssid->valuestring && j_ssid->valuestring[0]) {
        const char *pass = (cJSON_IsString(j_pass) && j_pass->valuestring) ? j_pass->valuestring : "";
        const char *url  = (cJSON_IsString(j_url)  && j_url->valuestring)  ? j_url->valuestring  : "";
        err = net_link_set_credentials(j_ssid->valuestring, pass, url);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "pre-carga importada desde %s", NET_CONFIG_PATH);
    } else {
        ESP_LOGW(TAG, "%s sin 'ssid' valido", NET_CONFIG_PATH);
    }

    cJSON_Delete(root);
    return err;
}

// ---------------------------------------------------------------------------
// Handlers de eventos WiFi/IP. Manejan asociacion, reintentos y obtencion de IP.
// ---------------------------------------------------------------------------
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        if (s_retry < NET_LINK_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "desconectado (reason=%d), reintento %d/%d",
                     d ? d->reason : -1, s_retry, NET_LINK_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_event_group, NL_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_event_group, NL_CONNECTED_BIT);
    }
}

// Registra (idempotente) los handlers de eventos WiFi/IP sobre on_event().
static void register_handlers(void)
{
    if (s_wifi_handler == NULL)
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &on_event, NULL, &s_wifi_handler);
    if (s_ip_handler == NULL)
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &on_event, NULL, &s_ip_handler);
}

// Espera asociacion + IP (o fallo/timeout). Compartido por ambos caminos de conexion.
static esp_err_t wait_for_ip(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, NL_CONNECTED_BIT | NL_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & NL_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi OK ip=%s rssi=%d", s_ip, net_link_get_rssi());
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi no conecto (%s)", (bits & NL_FAIL_BIT) ? "fallo" : "timeout");
    return ESP_FAIL;
}

esp_err_t net_link_connect(int timeout_ms)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (ensure_nvs() != ESP_OK)
        return ESP_FAIL;
    if (!nvs_get_str_field(NVS_K_SSID, ssid, sizeof(ssid))) {
        ESP_LOGW(TAG, "sin credenciales en NVS; se omite WiFi");
        return ESP_ERR_NOT_FOUND;
    }
    nvs_get_str_field(NVS_K_PASS, pass, sizeof(pass));        // puede faltar (red abierta)
    nvs_get_str_field(NVS_K_URL, s_base_url, sizeof(s_base_url));

    // --- netif + default event loop (nmea usa un loop propio, no el default). ---
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    if (s_event_group == NULL)
        s_event_group = xEventGroupCreate();
    else
        xEventGroupClearBits(s_event_group, NL_CONNECTED_BIT | NL_FAIL_BIT);

    if (s_netif == NULL)
        s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    register_handlers();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    // Si hay password asumimos al menos WPA2; si esta vacio, red abierta.
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_retry = 0;
    s_connected = false;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        net_link_stop();
        return err;
    }
    s_started = true;

    ESP_LOGI(TAG, "conectando a '%s' (timeout %d ms)...", ssid, timeout_ms);

    esp_err_t r = wait_for_ip(timeout_ms);
    if (r != ESP_OK)
        net_link_stop();
    return r;
}

// Escanea (bloqueante) buscando *ssid* y devuelve su canal primario, o 0 si no
// aparece. Necesario en APSTA: el radio es uno solo, asi que STA y SoftAP deben
// compartir canal. Sin esto, si la red del cliente esta en un canal distinto al
// del SoftAP (canal 1), la STA nunca asocia (auth -> init en loop).
static uint8_t scan_channel_for_ssid(const char *ssid)
{
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)ssid,
        .show_hidden = false,
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
        return 0;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0)
        return 0;
    if (n > 12)
        n = 12;
    wifi_ap_record_t recs[12];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK)
        return 0;

    for (uint16_t i = 0; i < n; i++) {
        if (strcmp((char *)recs[i].ssid, ssid) == 0)
            return recs[i].primary;
    }
    return 0;
}

esp_err_t net_link_sta_join_keep_ap(const char *ssid, const char *pass, int timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    if (pass == NULL)
        pass = "";

    // base_url para la Capa B (lo lee map_sync via net_link_get_base_url()).
    if (ensure_nvs() == ESP_OK)
        nvs_get_str_field(NVS_K_URL, s_base_url, sizeof(s_base_url));

    // El driver WiFi ya esta init+start por wifi_portal en APSTA. Solo preparamos
    // la interfaz STA: netif, event group, handlers, config — sin tocar el modo.
    if (s_event_group == NULL)
        s_event_group = xEventGroupCreate();
    else
        xEventGroupClearBits(s_event_group, NL_CONNECTED_BIT | NL_FAIL_BIT);

    // El netif de STA lo crea wifi_portal ANTES de esp_wifi_start (si se creara
    // aca, despues del start, el rx-path no queda atado y crashea al primer rx).
    register_handlers();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // Scan en todos los canales para encontrar la red aunque no este en el del AP.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    // APSTA = un solo radio: STA y SoftAP deben compartir canal. Buscamos el canal
    // real de la red del cliente y, si difiere del canal del AP, movemos el SoftAP
    // a ese canal (el celular se cae un instante y reconecta al mismo SSID abierto).
    uint8_t ch = scan_channel_for_ssid(ssid);
    if (ch) {
        ESP_LOGI(TAG, "'%s' esta en canal %d", ssid, ch);
        wifi_config_t apc;
        if (esp_wifi_get_config(WIFI_IF_AP, &apc) == ESP_OK && apc.ap.channel != ch) {
            apc.ap.channel = ch;
            if (esp_wifi_set_config(WIFI_IF_AP, &apc) == ESP_OK)
                ESP_LOGI(TAG, "SoftAP movido al canal %d (el celular reconecta solo)", ch);
        }
        wc.sta.channel = ch;
    } else {
        ESP_LOGW(TAG, "no se encontro '%s' en el scan; se intenta igual", ssid);
    }

    // NO cambiamos el modo (sigue APSTA: el AP/portal queda arriba). Solo
    // configuramos STA y disparamos la asociacion manualmente (el STA_START ya
    // ocurrio cuando el portal hizo esp_wifi_start, antes de registrar handlers).
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config STA: %s", esp_err_to_name(err));
        return err;
    }

    // Sin power-save: en APSTA, con el SoftAP sirviendo al celular, el power-save
    // del STA hace que la asociacion al AP del cliente expire (reason 4/205). Con
    // PS_NONE el STA escucha siempre y la asociacion se completa. Es una sesion
    // breve (bajar catalogo) y on-demand, asi que el consumo extra no importa.
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_retry = 0;
    s_connected = false;
    ESP_LOGI(TAG, "APSTA: asociando STA a '%s' (timeout %d ms)...", ssid, timeout_ms);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return err;
    }

    // En fallo NO hacemos teardown: el AP/portal sigue arriba (el llamador reinicia).
    return wait_for_ip(timeout_ms);
}

bool net_link_is_connected(void)
{
    return s_connected;
}

int net_link_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}

void net_link_get_ip(char *buf, size_t len)
{
    if (buf && len)
        strlcpy(buf, s_connected ? s_ip : "0.0.0.0", len);
}

const char *net_link_get_base_url(void)
{
    return s_base_url;
}

void net_link_stop(void)
{
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    esp_wifi_deinit();
    s_connected = false;
    strcpy(s_ip, "0.0.0.0");
}
