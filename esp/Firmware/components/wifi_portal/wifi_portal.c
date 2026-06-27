#include "wifi_portal.h"
#include "net_link.h"
#include "map_sync.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "lwip/sockets.h"

static const char *TAG = "WIFI_PORTAL";

#define AP_IP_ADDR      "192.168.4.1"
#define AP_MAX_CONN     2
#define AP_CHANNEL      1
#define SCAN_MAX_AP     16

#define PORTAL_SAVED_BIT   BIT0
#define MAP_PORTAL_MAX_ZONES  16

static EventGroupHandle_t s_portal_evt = NULL;
static httpd_handle_t     s_httpd      = NULL;
static volatile bool      s_dns_run    = false;

// --- Estado del flujo APSTA (Capa B): join STA + fetch del catalogo en vivo ---
typedef enum { PW_IDLE = 0, PW_CONNECTING, PW_OK, PW_FAIL } pw_state_t;
typedef enum { PC_NONE = 0, PC_PENDING, PC_OK, PC_FAIL } pc_state_t;
static volatile pw_state_t s_wifi_state = PW_IDLE;
static volatile pc_state_t s_cat_state  = PC_NONE;
static map_zone_t *s_zones   = NULL;   // catalogo bajado (publicado por el worker)
static int         s_zones_n = 0;
static char s_join_ssid[33] = {0};
static char s_join_pass[65] = {0};

#define STA_JOIN_TIMEOUT_MS  30000   // señal débil del cliente puede tardar

// ---------------------------------------------------------------------------
// Pagina HTML del portal (minima, sin dependencias externas).
// ---------------------------------------------------------------------------
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html lang=es><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Mykeego</title><style>"
"body{font-family:sans-serif;margin:0;padding:1.2em;background:#111;color:#eee}"
"h1{font-size:1.2em}select,input,button{width:100%;box-sizing:border-box;padding:.7em;margin:.4em 0;font-size:1em;border-radius:.4em;border:1px solid #444;background:#222;color:#eee}"
"button{background:#0a7;border:0;font-weight:bold}button:disabled{opacity:.5}"
"#msg{margin-top:.8em;min-height:1.2em}.z{display:flex;align-items:center;background:#222;border:1px solid #444;border-radius:.4em;padding:.6em;margin:.4em 0}"
".z input{width:auto;margin:0 .7em 0 0}.z label{flex:1}.z small{color:#999}.hide{display:none}"
"</style></head><body><h1>Configurar Mykeego</h1>"
"<div id=s1><select id=ssid><option>Buscando redes...</option></select>"
"<input id=pass type=password placeholder='Contrase\xc3\xb1" "a'>"
"<button id=bw onclick=wifi()>Conectar</button></div>"
"<div id=s2 class=hide><div id=zones></div>"
"<button id=bs onclick=save()>Guardar zonas</button></div>"
"<div id=msg></div>"
"<script>"
"var $=function(i){return document.getElementById(i)};"
"function scan(){fetch('/scan').then(r=>r.json()).then(l=>{var s=$('ssid');s.innerHTML='';"
"l.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;"
"o.textContent=n.ssid+' ('+n.rssi+'dBm)';s.appendChild(o);});"
"if(!l.length)s.innerHTML='<option>No se encontraron redes</option>';});}"
"function wifi(){$('bw').disabled=true;$('msg').textContent='Conectando...';"
"fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:$('ssid').value,password:$('pass').value})})"
".then(()=>poll());}"
"function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
"if(j.wifi=='fail'){$('msg').textContent='No se pudo conectar. Revis\xc3\xa1 la clave.';$('bw').disabled=false;return;}"
"if(j.wifi=='ok'&&j.catalog=='ok'){zones();return;}"
"if(j.wifi=='ok'&&j.catalog=='fail'){$('msg').textContent='Conect\xc3\xb3 pero no se pudo bajar el cat\xc3\xa1logo. Reintent\xc3\xa1.';$('bw').disabled=false;return;}"
"setTimeout(poll,1500);});}"
"function zones(){fetch('/zones').then(r=>r.json()).then(zs=>{var c=$('zones');c.innerHTML='';"
"zs.forEach(z=>{var kb=(z.bytes/1024).toFixed(0);c.innerHTML+="
"'<div class=z><input type=checkbox id=z_'+z.id+' value='+z.id+'>'+"
"'<label for=z_'+z.id+'>'+z.name+' <small>('+z.tiles+' tiles, '+kb+' KB)</small></label></div>';});"
"$('s1').classList.add('hide');$('s2').classList.remove('hide');"
"$('msg').textContent='Eleg\xc3\xad las zonas a descargar.';});}"
"function save(){var ids=[];document.querySelectorAll('#zones input:checked').forEach(e=>ids.push(e.value));"
"$('bs').disabled=true;$('msg').textContent='Guardando...';"
"fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({zones:ids})})"
".then(r=>r.text()).then(t=>{$('msg').textContent=t;});}"
"scan();"
"</script></body></html>";

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);   // bloqueante

    uint16_t n = SCAN_MAX_AP;
    wifi_ap_record_t recs[SCAN_MAX_AP];
    esp_wifi_scan_get_ap_records(&n, recs);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < n; i++) {
        if (recs[i].ssid[0] == '\0')
            continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body ? body : "[]");
    free(body);
    return ESP_OK;
}

// Lee el cuerpo de la request a un buffer NUL-terminado. Devuelve NULL en error
// (ya respondio 400). El llamador debe free().
static char *recv_body(httpd_req_t *req, size_t cap)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
        return NULL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

// Worker: asocia STA (manteniendo el AP) y baja el catalogo en vivo. Publica el
// estado en s_wifi_state / s_cat_state para que /status y /zones lo reporten.
static void join_worker(void *arg)
{
    s_wifi_state = PW_CONNECTING;
    s_cat_state  = PC_PENDING;

    esp_err_t err = net_link_sta_join_keep_ap(s_join_ssid, s_join_pass, STA_JOIN_TIMEOUT_MS);
    if (err != ESP_OK) {
        s_wifi_state = PW_FAIL;
        s_cat_state  = PC_NONE;
        vTaskDelete(NULL);
        return;
    }
    s_wifi_state = PW_OK;

    map_zone_t *z = NULL;
    int n = 0;
    if (map_sync_fetch_catalog(&z, &n) == ESP_OK) {
        if (s_zones) map_sync_free_catalog(s_zones);
        s_zones   = z;
        s_zones_n = n;
        s_cat_state = PC_OK;   // publicar despues de setear el puntero
    } else {
        s_cat_state = PC_FAIL;
    }
    vTaskDelete(NULL);
}

// POST /wifi {ssid,password}: guarda creds y dispara el worker (join + catalogo).
static esp_err_t h_wifi(httpd_req_t *req)
{
    char *buf = recv_body(req, 256);
    if (buf == NULL)
        return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (!cJSON_IsString(j_ssid) || !j_ssid->valuestring || !j_ssid->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
        return ESP_FAIL;
    }
    const char *pass = (cJSON_IsString(j_pass) && j_pass->valuestring) ? j_pass->valuestring : "";

    // Guarda credenciales (conserva el base_url) y prepara el join.
    net_link_set_credentials(j_ssid->valuestring, pass, net_link_get_base_url());
    strlcpy(s_join_ssid, j_ssid->valuestring, sizeof(s_join_ssid));
    strlcpy(s_join_pass, pass, sizeof(s_join_pass));
    cJSON_Delete(root);

    // Evita lanzar dos workers en paralelo si el cliente reintenta.
    // Stack holgado: el worker hace handshake TLS (mbedTLS) + parseo del catalogo.
    if (s_wifi_state != PW_CONNECTING)
        xTaskCreate(join_worker, "portal_join", 8192, NULL, 4, NULL);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// GET /status: {wifi:"...",catalog:"...",ip:"..."} para que la pagina poolee.
static esp_err_t h_status(httpd_req_t *req)
{
    const char *w = s_wifi_state == PW_OK ? "ok"
                  : s_wifi_state == PW_FAIL ? "fail"
                  : s_wifi_state == PW_CONNECTING ? "connecting" : "idle";
    const char *c = s_cat_state == PC_OK ? "ok"
                  : s_cat_state == PC_FAIL ? "fail"
                  : s_cat_state == PC_PENDING ? "pending" : "none";
    char ip[16];
    net_link_get_ip(ip, sizeof(ip));

    char body[96];
    snprintf(body, sizeof(body),
             "{\"wifi\":\"%s\",\"catalog\":\"%s\",\"ip\":\"%s\"}", w, c, ip);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

// GET /zones: catalogo bajado, serializado para la pagina.
static esp_err_t h_zones(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (s_cat_state != PC_OK || s_zones == NULL) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_zones_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_zones[i].id);
        cJSON_AddStringToObject(o, "name", s_zones[i].name);
        cJSON_AddNumberToObject(o, "version", s_zones[i].version);
        cJSON_AddNumberToObject(o, "tiles", s_zones[i].tiles);
        cJSON_AddNumberToObject(o, "bytes", s_zones[i].bytes);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_sendstr(req, body ? body : "[]");
    free(body);
    return ESP_OK;
}

// POST /save {zones:[...]}: persiste las zonas elegidas y marca fin del portal.
static esp_err_t h_save(httpd_req_t *req)
{
    char *buf = recv_body(req, 512);
    if (buf == NULL)
        return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    const cJSON *j_zones = cJSON_GetObjectItemCaseSensitive(root, "zones");

    const char *ids[MAP_PORTAL_MAX_ZONES];
    int n = 0;
    if (cJSON_IsArray(j_zones)) {
        const cJSON *z;
        cJSON_ArrayForEach(z, j_zones) {
            if (n >= MAP_PORTAL_MAX_ZONES)
                break;
            if (cJSON_IsString(z) && z->valuestring && z->valuestring[0])
                ids[n++] = z->valuestring;
        }
    }
    esp_err_t err = map_sync_set_selected(ids, n);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "Guardado. El equipo se reiniciara y descargara los mapas.");
        xEventGroupSetBits(s_portal_evt, PORTAL_SAVED_BIT);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    return ESP_FAIL;
}

// Catch-all: cualquier otra URL (probes de captive portal) redirige al portal.
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_ADDR "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// DNS hijack: responde la IP del AP a toda consulta A (dispara el captive portal)
// ---------------------------------------------------------------------------
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "dns socket fail");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "dns bind fail");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (len < (int)sizeof(uint16_t) * 6)
            continue;   // timeout o paquete invalido

        // Construir respuesta: copiar query, marcar como respuesta, 1 answer.
        pkt[2] = 0x81; pkt[3] = 0x80;            // flags: response, recursion avail
        pkt[6] = 0x00; pkt[7] = 0x01;            // ANCOUNT = 1
        // (QDCOUNT ya viene = 1 en la query)

        if (len + 16 > (int)sizeof(pkt))
            continue;
        uint8_t *p = pkt + len;
        *p++ = 0xC0; *p++ = 0x0C;                // pointer al nombre de la query
        *p++ = 0x00; *p++ = 0x01;                // TYPE A
        *p++ = 0x00; *p++ = 0x01;                // CLASS IN
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL 60
        *p++ = 0x00; *p++ = 0x04;                // RDLENGTH 4
        uint32_t ip = inet_addr(AP_IP_ADDR);     // 192.168.4.1
        memcpy(p, &ip, 4); p += 4;

        sendto(sock, pkt, p - pkt, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Bring-up del AP + httpd + DNS
// ---------------------------------------------------------------------------
void wifi_portal_get_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "Mykeego-%02X%02X", mac[4], mac[5]);
}

static esp_err_t start_softap(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_ap();
    // Crear TAMBIEN el netif de STA aca (antes de esp_wifi_start) para que el
    // rx-path del driver quede atado y arranque DHCP al conectar. Si se crea tarde
    // (dentro de net_link_sta_join_keep_ap, despues del start), el primer paquete
    // recibido entra a un netif sin callback -> crash (InstrFetchProhibited, PC=0).
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    // SSID "Mykeego-XXXX" con sufijo del MAC.
    wifi_config_t apc = { 0 };
    wifi_portal_get_ap_ssid((char *)apc.ap.ssid, sizeof(apc.ap.ssid));
    apc.ap.ssid_len = strlen((char *)apc.ap.ssid);
    apc.ap.channel = AP_CHANNEL;
    apc.ap.max_connection = AP_MAX_CONN;
    apc.ap.authmode = WIFI_AUTH_OPEN;   // portal abierto; autostop por timeout

    // APSTA: el AP sirve el portal y la interfaz STA permite escanear redes.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP '%s' arriba en %s", apc.ap.ssid, AP_IP_ADDR);

    // httpd
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = 8;
    err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) return err;

    httpd_uri_t u_root   = { .uri = "/", .method = HTTP_GET, .handler = h_root };
    httpd_uri_t u_scan   = { .uri = "/scan", .method = HTTP_GET, .handler = h_scan };
    httpd_uri_t u_wifi   = { .uri = "/wifi", .method = HTTP_POST, .handler = h_wifi };
    httpd_uri_t u_status = { .uri = "/status", .method = HTTP_GET, .handler = h_status };
    httpd_uri_t u_zones  = { .uri = "/zones", .method = HTTP_GET, .handler = h_zones };
    httpd_uri_t u_save   = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u_any    = { .uri = "/*", .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_wifi);
    httpd_register_uri_handler(s_httpd, &u_status);
    httpd_register_uri_handler(s_httpd, &u_zones);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_any);

    // DNS hijack
    s_dns_run = true;
    xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 4, NULL);
    return ESP_OK;
}

// NOTA: a proposito NO hay teardown graceful de WiFi aca. El esp_wifi_deinit()
// dispara escrituras a flash (NVS/phy) que deshabilitan la cache, y como los
// buffers de LVGL viven en PSRAM, un flush concurrente fallaba ('spi transmit
// color failed') y colgaba LVGL -> watchdog. El llamador siempre reinicia el
// equipo despues de provisionar (con LVGL parqueado), y el reset limpia WiFi.
esp_err_t wifi_portal_run(int timeout_ms)
{
    if (s_portal_evt == NULL)
        s_portal_evt = xEventGroupCreate();
    else
        xEventGroupClearBits(s_portal_evt, PORTAL_SAVED_BIT);

    esp_err_t err = start_softap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_softap: %s", esp_err_to_name(err));
        return err;   // el llamador reinicia; el reset limpia lo que haya quedado
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_portal_evt, PORTAL_SAVED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    // Pequena espera para que la respuesta HTTP de "/save" llegue al celular
    // antes de que el llamador reinicie.
    if (bits & PORTAL_SAVED_BIT)
        vTaskDelay(pdMS_TO_TICKS(500));

    return (bits & PORTAL_SAVED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
