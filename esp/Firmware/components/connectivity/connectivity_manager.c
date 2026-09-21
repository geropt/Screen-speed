#include "connectivity_manager.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CONN";

#define NVS_NAMESPACE "conn_nets"

#define BIT_GOT_IP    BIT0
#define BIT_CANCELLED BIT1

typedef struct {
    char ssid[CONN_SSID_MAX + 1];
    char pass[CONN_PASS_MAX + 1];
} network_t;

static struct {
    bool             inited;
    conn_config_t    cfg;
    conn_state_t     state;
    EventGroupHandle_t events;
    SemaphoreHandle_t  mux;
    esp_netif_t       *netif;
    bool             requested[CONN_CONSUMER_COUNT];
    network_t        nets[CONN_MAX_NETWORKS];
    size_t           net_count;
    size_t           next_net;      /* red a probar en el próximo intento */
    uint32_t         backoff_ms;
    uint32_t         attempts;
    uint32_t         failures;
    uint32_t         disconnects;
    bool             have_ip;
} s;

static uint8_t active_requests(void)
{
    uint8_t n = 0;
    for (int i = 0; i < CONN_CONSUMER_COUNT; ++i) {
        if (s.requested[i]) {
            n++;
        }
    }
    return n;
}

/* ---------- persistencia por red ----------
 * Cada red es una clave propia. Nunca se hace `nvs_flash_erase()`: el piloto borra
 * todo el NVS y el plan lo prohíbe para el producto, porque se llevaría puesta
 * cualquier otra cosa guardada ahí. */

static void nvs_save_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no se pudo abrir NVS para guardar redes");
        return;
    }
    /* Se reescriben las entradas vigentes y se borran sólo los índices sobrantes. */
    for (size_t i = 0; i < CONN_MAX_NETWORKS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "net%u", (unsigned)i);
        if (i < s.net_count) {
            nvs_set_blob(h, key, &s.nets[i], sizeof(network_t));
        } else {
            nvs_erase_key(h, key);   /* puede no existir; el error no importa */
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   /* primera vez: no hay nada guardado */
    }
    s.net_count = 0;
    for (size_t i = 0; i < CONN_MAX_NETWORKS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "net%u", (unsigned)i);
        size_t len = sizeof(network_t);
        if (nvs_get_blob(h, key, &s.nets[s.net_count], &len) == ESP_OK &&
            len == sizeof(network_t)) {
            s.net_count++;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%u redes cargadas de NVS", (unsigned)s.net_count);
}

/* ---------- eventos ---------- */

static void try_connect(void);

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        try_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.have_ip = false;
        xEventGroupClearBits(s.events, BIT_GOT_IP);
        if (s.state == CONN_STATE_CONNECTED) {
            s.disconnects++;
            ESP_LOGW(TAG, "desconectado");
        } else {
            s.failures++;
        }
        if (active_requests() == 0) {
            s.state = CONN_STATE_OFF;
            return;
        }
        /* Backoff con techo: reintentar sin pausa contra un AP que no responde gasta
         * energía y llena el log. */
        s.state = CONN_STATE_BACKOFF;
        ESP_LOGW(TAG, "reintento en %" PRIu32 " ms", s.backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(s.backoff_ms));
        if (s.backoff_ms < s.cfg.backoff_max_ms) {
            s.backoff_ms *= 2;
            if (s.backoff_ms > s.cfg.backoff_max_ms) {
                s.backoff_ms = s.cfg.backoff_max_ms;
            }
        }
        /* Probar la siguiente red guardada: puede que la anterior no esté al alcance. */
        if (s.net_count > 1) {
            s.next_net = (s.next_net + 1) % s.net_count;
        }
        if (active_requests() > 0) {
            try_connect();
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        s.have_ip = true;
        s.state = CONN_STATE_CONNECTED;
        /* Éxito: el backoff vuelve al valor inicial. */
        s.backoff_ms = s.cfg.backoff_initial_ms;
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s.events, BIT_GOT_IP);
    }
}

static void try_connect(void)
{
    if (s.net_count == 0) {
        s.state = CONN_STATE_NO_CONFIG;
        ESP_LOGW(TAG, "no hay redes configuradas");
        return;
    }
    const network_t *n = &s.nets[s.next_net % s.net_count];

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, n->ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, n->pass, sizeof(wc.sta.password) - 1);

    s.state = CONN_STATE_CONNECTING;
    s.attempts++;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

/* ---------- API ---------- */

esp_err_t conn_init(const conn_config_t *cfg)
{
    if (s.inited) {
        return ESP_OK;
    }
    memset(&s, 0, sizeof(s));
    if (cfg) {
        s.cfg = *cfg;
    } else {
        conn_config_t d = CONN_CONFIG_DEFAULT();
        s.cfg = d;
    }
    s.backoff_ms = s.cfg.backoff_initial_ms;

    s.events = xEventGroupCreate();
    s.mux = xSemaphoreCreateMutex();
    if (!s.events || !s.mux) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Se borra SOLO la partición de NVS cuando está inutilizable, que es el
         * único caso en que no hay alternativa. No es un borrado de rutina. */
        ESP_LOGW(TAG, "NVS inutilizable, reinicializando la particion");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_load_all();

    /* Nada de radio todavía: se levanta con el primer pedido. Un HUD que nunca
     * necesita red no debería gastar energía en Wi-Fi. */
    s.state = (s.net_count == 0) ? CONN_STATE_NO_CONFIG : CONN_STATE_OFF;
    s.inited = true;
    ESP_LOGI(TAG, "listo, radio apagada, %u redes", (unsigned)s.net_count);
    return ESP_OK;
}

esp_err_t conn_add_network(const char *ssid, const char *password)
{
    if (!s.inited || !ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > CONN_SSID_MAX ||
        (password && strlen(password) > CONN_PASS_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s.mux, portMAX_DELAY);

    /* Reemplazo si el SSID ya existe. */
    for (size_t i = 0; i < s.net_count; ++i) {
        if (strcmp(s.nets[i].ssid, ssid) == 0) {
            strncpy(s.nets[i].pass, password ? password : "", CONN_PASS_MAX);
            s.nets[i].pass[CONN_PASS_MAX] = '\0';
            nvs_save_all();
            xSemaphoreGive(s.mux);
            return ESP_OK;
        }
    }
    if (s.net_count >= CONN_MAX_NETWORKS) {
        xSemaphoreGive(s.mux);
        return ESP_ERR_NO_MEM;
    }
    network_t *n = &s.nets[s.net_count];
    memset(n, 0, sizeof(*n));
    strncpy(n->ssid, ssid, CONN_SSID_MAX);
    strncpy(n->pass, password ? password : "", CONN_PASS_MAX);
    s.net_count++;
    nvs_save_all();
    if (s.state == CONN_STATE_NO_CONFIG) {
        s.state = CONN_STATE_OFF;
    }
    xSemaphoreGive(s.mux);
    ESP_LOGI(TAG, "red agregada, total %u", (unsigned)s.net_count);
    return ESP_OK;
}

esp_err_t conn_forget_network(const char *ssid)
{
    if (!s.inited || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s.mux, portMAX_DELAY);
    for (size_t i = 0; i < s.net_count; ++i) {
        if (strcmp(s.nets[i].ssid, ssid) == 0) {
            for (size_t j = i; j + 1 < s.net_count; ++j) {
                s.nets[j] = s.nets[j + 1];
            }
            s.net_count--;
            memset(&s.nets[s.net_count], 0, sizeof(network_t));
            nvs_save_all();
            xSemaphoreGive(s.mux);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s.mux);
    return ESP_ERR_NOT_FOUND;
}

size_t conn_network_count(void)
{
    return s.net_count;
}

static esp_err_t radio_up(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s.netif) {
        s.netif = esp_netif_create_default_wifi_sta();
        if (!s.netif) {
            return ESP_FAIL;
        }
    }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             on_wifi_event, NULL, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             on_ip_event, NULL, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_start();   /* dispara STA_START → try_connect() */
}

esp_err_t conn_request(conn_consumer_t who)
{
    if (!s.inited || who >= CONN_CONSUMER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s.mux, portMAX_DELAY);
    bool was_idle = (active_requests() == 0);
    if (s.requested[who]) {
        xSemaphoreGive(s.mux);
        return ESP_OK;   /* idempotente por consumidor */
    }
    s.requested[who] = true;
    ESP_LOGI(TAG, "%s pidió conexión (%u activos)",
             conn_consumer_name(who), (unsigned)active_requests());
    xSemaphoreGive(s.mux);

    if (was_idle) {
        s.backoff_ms = s.cfg.backoff_initial_ms;
        return radio_up();
    }
    return ESP_OK;
}

esp_err_t conn_release(conn_consumer_t who)
{
    if (!s.inited || who >= CONN_CONSUMER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s.mux, portMAX_DELAY);
    if (!s.requested[who]) {
        xSemaphoreGive(s.mux);
        return ESP_OK;
    }
    s.requested[who] = false;
    uint8_t left = active_requests();
    xSemaphoreGive(s.mux);

    ESP_LOGI(TAG, "%s liberó (%u activos)", conn_consumer_name(who), (unsigned)left);
    if (left > 0) {
        /* Otro consumidor sigue usando la radio: no se apaga. Esto es lo que evita
         * que el respaldo corte una transferencia de mapas en curso. */
        return ESP_OK;
    }
    s.have_ip = false;
    xEventGroupClearBits(s.events, BIT_GOT_IP);
    s.state = CONN_STATE_OFF;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t conn_wait_connected(uint32_t timeout_ms)
{
    if (!s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xEventGroupGetBits(s.events) & BIT_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s.events, BIT_GOT_IP | BIT_CANCELLED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CANCELLED) {
        /* Cancelación explícita: se vuelve de inmediato en lugar de aguantar el
         * timeout del stack. Es lo que hace cancelable la espera. */
        return ESP_ERR_INVALID_STATE;
    }
    if (bits & BIT_GOT_IP) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void conn_cancel_waits(void)
{
    if (s.events) {
        xEventGroupSetBits(s.events, BIT_CANCELLED);
    }
}

void conn_resume_waits(void)
{
    if (s.events) {
        xEventGroupClearBits(s.events, BIT_CANCELLED);
    }
}

bool conn_waits_cancelled(void)
{
    return s.events && (xEventGroupGetBits(s.events) & BIT_CANCELLED) != 0;
}

void conn_get_status(conn_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = s.state;
    out->attempts = s.attempts;
    out->failures = s.failures;
    out->disconnects = s.disconnects;
    out->current_backoff_ms = s.backoff_ms;
    out->active_requests = active_requests();
    out->have_ip = s.have_ip;
}

const char *conn_state_name(conn_state_t st)
{
    switch (st) {
    case CONN_STATE_OFF:        return "apagada";
    case CONN_STATE_CONNECTING: return "conectando";
    case CONN_STATE_CONNECTED:  return "conectada";
    case CONN_STATE_BACKOFF:    return "backoff";
    case CONN_STATE_NO_CONFIG:  return "sin_configurar";
    default:                    return "?";
    }
}

const char *conn_consumer_name(conn_consumer_t c)
{
    switch (c) {
    case CONN_CONSUMER_BACKUP: return "respaldo";
    case CONN_CONSUMER_MAPS:   return "mapas";
    case CONN_CONSUMER_OTA:    return "ota";
    case CONN_CONSUMER_DIAG:   return "diagnostico";
    default:                   return "?";
    }
}
