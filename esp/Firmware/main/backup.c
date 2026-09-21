#include "backup.h"

#if CONFIG_BACKUP_ENABLE

#include "backup_policy.h"
#include "connectivity_manager.h"
#include "outbox.h"
#include "res_metrics.h"
#include "ruptela_io_parser.h"
#include "ruptela_proto.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "BACKUP";

#define REC_QUEUE_LEN 16
#define HEARTBEAT_MS  60000
#define ACK_WAIT_MS   5000
#define RECV_CHUNK_MS 200

typedef struct {
    uint8_t rec[OUTBOX_RECORD_MAX];
    uint8_t len;
} rec_item_t;

#define BACKUP_NVS_NS "backup"

static QueueHandle_t s_rec_q;
static SemaphoreHandle_t s_in_mux;
static SemaphoreHandle_t s_ep_mux;
static backup_inputs_t s_inputs;
static bool s_have_inputs;
static bool s_started;

static char s_host[BACKUP_CFG_HOST_MAX + 1];
static uint16_t s_port;
static bool s_ep_dirty;

static outbox_t s_outbox;
static backup_policy_t s_policy;
static int s_sock = -1;
static bool s_radio_held;
static TickType_t s_last_tx;
static uint64_t s_inflight_id;
static backup_verdict_t s_last_verdict = BACKUP_DENY_GPRS_UNKNOWN;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t imei_u64(const char *digits)
{
    if (!digits || digits[0] == '\0') {
        return 0;
    }
    return strtoull(digits, NULL, 10);
}

static void close_socket(void)
{
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
        ESP_LOGI(TAG, "TCP cerrado");
    }
}

static void on_record(void *ctx, const uint8_t *record, size_t len)
{
    (void)ctx;
    if (!record || len == 0 || len > OUTBOX_RECORD_MAX) {
        return;
    }
    if (!ruptela_io_record_can_tx(record, len)) {
        return;
    }
    rec_item_t item = {0};
    memcpy(item.rec, record, len);
    item.len = (uint8_t)len;
    if (xQueueSend(s_rec_q, &item, 0) != pdTRUE) {
        res_metrics_error(RES_ERR_QUEUE_FULL);
    }
}

void backup_publish_snapshot(const vehicle_snapshot_t *snap)
{
    if (!snap || !s_in_mux) {
        return;
    }
    backup_inputs_t in = {0};
    in.imei_known = snap->imei_known;
    if (snap->imei_known) {
        strncpy(in.imei, snap->imei, BACKUP_IMEI_MAX);
        in.imei[BACKUP_IMEI_MAX] = '\0';
    }
    in.gprs_known = (snap->gprs_state != VS_UNKNOWN);
    in.gprs_up = snap->gprs_up;
    in.gprs_age_ms = snap->gprs_age_ms;
    in.ignition_known = (snap->ignition_state != VS_UNKNOWN);
    in.ignition_on = snap->ignition_on;

#if CONFIG_BACKUP_LAB_ASSUME_GPRS_DOWN
    /* Banco con Flipper: el .log casi no transporta IO 418. Sin esto la política
     * niega siempre (GPRS desconocido) y el Wi-Fi nunca se levanta. En un
     * vehículo real hay que apagar esta opción. */
    if (!in.gprs_known) {
        in.gprs_known = true;
        in.gprs_up = false;
        in.gprs_age_ms = 0;
    }
#endif

    if (xSemaphoreTake(s_in_mux, 0) == pdTRUE) {
        s_inputs = in;
        s_have_inputs = true;
        xSemaphoreGive(s_in_mux);
    }
}

static bool copy_inputs(backup_inputs_t *out)
{
    if (!s_in_mux) {
        return false;
    }
    if (xSemaphoreTake(s_in_mux, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    bool ok = s_have_inputs;
    if (ok) {
        *out = s_inputs;
    }
    xSemaphoreGive(s_in_mux);
    return ok;
}

static backup_verdict_t evaluate(backup_inputs_t *in_out)
{
    if (!copy_inputs(in_out)) {
        return s_last_verdict;
    }
    backup_verdict_t v = backup_policy_evaluate(&s_policy, now_ms(), in_out);
    if (v != s_last_verdict) {
        ESP_LOGI(TAG, "permiso=%s granted=%d pending=%u inflight=%u",
                 backup_verdict_name(v),
                 (int)backup_policy_is_granted(&s_policy),
                 (unsigned)outbox_pending(&s_outbox),
                 (unsigned)outbox_inflight(&s_outbox));
        s_last_verdict = v;
    }
    return v;
}

static void drain_records(void)
{
    rec_item_t item;
    uint64_t t = now_ms();
    while (xQueueReceive(s_rec_q, &item, 0) == pdTRUE) {
        if (!outbox_push(&s_outbox, t, item.rec, item.len)) {
            ESP_LOGW(TAG, "outbox no aceptó record len=%u", (unsigned)item.len);
        }
    }
}

static void abandon_inflight(void)
{
    if (s_inflight_id != 0) {
        outbox_release_batch(&s_outbox, s_inflight_id);
        s_inflight_id = 0;
    }
}

static void release_radio(void)
{
    close_socket();
    abandon_inflight();
    if (s_radio_held) {
        conn_cancel_waits();
        conn_release(CONN_CONSUMER_BACKUP);
        s_radio_held = false;
        conn_resume_waits();
    }
}

static bool still_allowed(void)
{
    backup_inputs_t in;
    evaluate(&in);
    return backup_policy_is_granted(&s_policy) &&
           !backup_policy_revocation_pending(&s_policy) &&
           !conn_waits_cancelled();
}

static void endpoint_from_kconfig(void)
{
    strncpy(s_host, CONFIG_BACKUP_HOST, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port = (uint16_t)CONFIG_BACKUP_PORT;
}

static void endpoint_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BACKUP_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char host[BACKUP_CFG_HOST_MAX + 1];
    size_t len = sizeof(host);
    if (nvs_get_str(h, "host", host, &len) == ESP_OK && host[0] != '\0') {
        strncpy(s_host, host, sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
    }
    uint16_t port = 0;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port >= 1) {
        s_port = port;
    }
    nvs_close(h);
}

static void endpoint_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BACKUP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no se pudo guardar endpoint en NVS");
        return;
    }
    nvs_set_str(h, "host", s_host);
    nvs_set_u16(h, "port", s_port);
    nvs_commit(h);
    nvs_close(h);
}

static void seed_kconfig_network(const char *ssid, const char *pass)
{
    /* Password vacío no pisa NVS: el plan prohíbe credenciales de build y
     * un flash sin clave no debe borrar la red ya provisionada. */
    if (!ssid || ssid[0] == '\0' || !pass || pass[0] == '\0') {
        return;
    }
    conn_add_network(ssid, pass);
}

static void copy_endpoint(char *host, size_t host_sz, uint16_t *port)
{
    if (s_ep_mux && xSemaphoreTake(s_ep_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(host, s_host, host_sz - 1);
        host[host_sz - 1] = '\0';
        *port = s_port;
        xSemaphoreGive(s_ep_mux);
        return;
    }
    strncpy(host, s_host, host_sz - 1);
    host[host_sz - 1] = '\0';
    *port = s_port;
}

static void honor_endpoint_change(void)
{
    if (!s_ep_mux) {
        return;
    }
    if (xSemaphoreTake(s_ep_mux, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    bool dirty = s_ep_dirty;
    if (dirty) {
        s_ep_dirty = false;
    }
    xSemaphoreGive(s_ep_mux);
    if (dirty && s_sock >= 0) {
        ESP_LOGI(TAG, "endpoint cambió, cierro TCP");
        close_socket();
    }
}

static bool tcp_ensure(uint64_t imei)
{
    if (s_sock >= 0) {
        return true;
    }
    char host[BACKUP_CFG_HOST_MAX + 1];
    uint16_t port_n = 0;
    copy_endpoint(host, sizeof(host), &port_n);
    if (host[0] == '\0' || port_n == 0) {
        ESP_LOGW(TAG, "sin host/puerto configurados");
        return false;
    }
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)port_n);
    if (!still_allowed()) {
        return false;
    }
    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGW(TAG, "DNS %s falló: %d", host, err);
        return false;
    }
    if (!still_allowed()) {
        freeaddrinfo(res);
        return false;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return false;
    }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect %s:%u errno=%d", host, (unsigned)port_n, errno);
        close(sock);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    s_sock = sock;
    s_last_tx = xTaskGetTickCount();
    ESP_LOGI(TAG, "TCP %s:%u IMEI=%" PRIu64, host, (unsigned)port_n, imei);
    return true;
}

static bool send_all(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (!still_allowed()) {
            return false;
        }
        int n = send(s_sock, buf + off, len - off, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "send errno=%d", errno);
            close_socket();
            return false;
        }
        off += (size_t)n;
    }
    s_last_tx = xTaskGetTickCount();
    return true;
}

static int wait_ack_records(uint32_t timeout_ms)
{
    uint8_t rx[256];
    size_t filled = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        if (!still_allowed() || s_sock < 0) {
            return -1;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = RECV_CHUNK_MS * 1000 };
        setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recv(s_sock, rx + filled, sizeof(rx) - filled, 0);
        if (n > 0) {
            filled += (size_t)n;
            size_t off = 0;
            while (off < filled) {
                ruptela_server_msg_t msg;
                size_t used = ruptela_parse_server_packet(rx + off, filled - off, &msg);
                if (used == 0) {
                    break;
                }
                ESP_LOGI(TAG, "RX cmd %u payload %u", msg.cmd, (unsigned)msg.payload_len);
                if (ruptela_cmd_must_drop(msg.cmd)) {
                    ESP_LOGW(TAG, "cmd %u no es para el HUD, cierro", msg.cmd);
                    close_socket();
                    return -1;
                }
                off += used;
                if (msg.cmd == RUPTELA_CMD_ACK_RECORDS) {
                    return 1;
                }
            }
            if (off > 0 && off < filled) {
                memmove(rx, rx + off, filled - off);
                filled -= off;
            } else if (off >= filled) {
                filled = 0;
            }
            if (filled == sizeof(rx)) {
                ESP_LOGW(TAG, "RX sin paquete parseable, descarto");
                filled = 0;
            }
        } else if (n == 0) {
            close_socket();
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close_socket();
            return -1;
        }
    }
    return 0;
}

static bool send_batch(uint64_t imei)
{
    outbox_batch_t batch;
    if (!outbox_begin_batch(&s_outbox, now_ms(), OUTBOX_BATCH_MAX, &batch)) {
        return true;
    }
    s_inflight_id = batch.batch_id;

    size_t lens[OUTBOX_BATCH_MAX];
    for (uint32_t i = 0; i < batch.count; ++i) {
        lens[i] = batch.lengths[i];
    }
    uint8_t pkt[RUPTELA_MAX_PACKET];
    size_t pkt_len = ruptela_build_cmd68(pkt, sizeof(pkt), imei,
                                         batch.records, lens, batch.count, 0);
    if (pkt_len == 0) {
        ESP_LOGW(TAG, "cmd68 build falló");
        outbox_release_batch(&s_outbox, batch.batch_id);
        s_inflight_id = 0;
        return false;
    }
    ESP_LOGI(TAG, "TX cmd68 %u rec %u B", (unsigned)batch.count, (unsigned)pkt_len);
    if (!send_all(pkt, pkt_len)) {
        outbox_release_batch(&s_outbox, batch.batch_id);
        s_inflight_id = 0;
        return false;
    }
    int ack = wait_ack_records(ACK_WAIT_MS);
    if (ack > 0) {
        outbox_confirm_batch(&s_outbox, batch.batch_id);
        s_inflight_id = 0;
        return true;
    }
    ESP_LOGW(TAG, "ACK cmd68 no llegó (%d); lote vuelve a pendiente", ack);
    outbox_release_batch(&s_outbox, batch.batch_id);
    s_inflight_id = 0;
    return false;
}

static bool send_heartbeat(uint64_t imei)
{
    uint8_t pkt[16];
    size_t n = ruptela_build_heartbeat(pkt, sizeof(pkt), imei);
    if (n == 0) {
        return false;
    }
    ESP_LOGI(TAG, "TX heartbeat 16");
    return send_all(pkt, n);
}

static void backup_task(void *arg)
{
    (void)arg;
    res_metrics_watch_task("backup", NULL);

    while (1) {
        drain_records();
        honor_endpoint_change();

        backup_inputs_t in;
        backup_verdict_t v = evaluate(&in);
        (void)v;

        if (backup_policy_revocation_pending(&s_policy) ||
            !backup_policy_is_granted(&s_policy)) {
            if (s_radio_held || s_sock >= 0 || s_inflight_id != 0) {
                release_radio();
                if (backup_policy_revocation_pending(&s_policy)) {
                    uint32_t took = backup_policy_ack_revocation(&s_policy, now_ms());
                    ESP_LOGI(TAG, "revocación atendida en %" PRIu32 " ms", took);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!s_radio_held) {
            if (conn_request(CONN_CONSUMER_BACKUP) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            s_radio_held = true;
        }
        if (conn_wait_connected(2000) != ESP_OK) {
            conn_status_t st;
            conn_get_status(&st);
            ESP_LOGW(TAG, "sin IP: %s", conn_state_name(st.state));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint64_t imei = imei_u64(in.imei);
        if (!tcp_ensure(imei)) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!send_batch(imei)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (s_sock >= 0 &&
            (xTaskGetTickCount() - s_last_tx) >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
            send_heartbeat(imei);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t backup_apply_cfg(const backup_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || !s_ep_mux) {
        return ESP_ERR_INVALID_STATE;
    }

    bool changed = false;
    xSemaphoreTake(s_ep_mux, portMAX_DELAY);
    if (cfg->have_host && strcmp(s_host, cfg->host) != 0) {
        strncpy(s_host, cfg->host, sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
        s_ep_dirty = true;
        changed = true;
    }
    if (cfg->have_port && s_port != cfg->port) {
        s_port = cfg->port;
        s_ep_dirty = true;
        changed = true;
    }
    if (cfg->have_host || cfg->have_port) {
        endpoint_save_nvs();
    }
    char host_log[BACKUP_CFG_HOST_MAX + 1];
    uint16_t port_log = s_port;
    strncpy(host_log, s_host, sizeof(host_log) - 1);
    host_log[sizeof(host_log) - 1] = '\0';
    xSemaphoreGive(s_ep_mux);

    for (size_t i = 0; i < cfg->net_count; i++) {
        if (cfg->nets[i].ssid[0] == '\0' || !cfg->nets[i].have_pass) {
            continue;
        }
        conn_add_network(cfg->nets[i].ssid, cfg->nets[i].pass);
        ESP_LOGI(TAG, "red %s actualizada", cfg->nets[i].ssid);
    }
    ESP_LOGI(TAG, "config aplicada host=%s port=%u redes_archivo=%u%s",
             host_log, (unsigned)port_log, (unsigned)cfg->net_count,
             changed ? " (endpoint nuevo)" : "");
    return ESP_OK;
}

esp_err_t backup_start(nmea_parser_handle_t nmea_hdl)
{
    s_rec_q = xQueueCreate(REC_QUEUE_LEN, sizeof(rec_item_t));
    s_in_mux = xSemaphoreCreateMutex();
    s_ep_mux = xSemaphoreCreateMutex();
    if (!s_rec_q || !s_in_mux || !s_ep_mux) {
        return ESP_ERR_NO_MEM;
    }

    outbox_config_t ob = OUTBOX_CONFIG_DEFAULT();
    outbox_init(&s_outbox, &ob);

    backup_policy_config_t pcfg = BACKUP_POLICY_CONFIG_DEFAULT();
    pcfg.dwell_ms = CONFIG_BACKUP_DWELL_MS;
#if CONFIG_BACKUP_REQUIRE_IGNITION
    pcfg.require_ignition = true;
    pcfg.allow_unknown_ignition = false;
#else
    pcfg.require_ignition = false;
    pcfg.allow_unknown_ignition = true;
#endif
    backup_policy_init(&s_policy, &pcfg);

    endpoint_from_kconfig();

    esp_err_t err = conn_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "conn_init: %s", esp_err_to_name(err));
        return err;
    }
    endpoint_load_nvs();
    seed_kconfig_network(CONFIG_BACKUP_WIFI_SSID, CONFIG_BACKUP_WIFI_PASSWORD);
    seed_kconfig_network(CONFIG_BACKUP_WIFI_SSID_2, CONFIG_BACKUP_WIFI_PASSWORD_2);
    seed_kconfig_network(CONFIG_BACKUP_WIFI_SSID_3, CONFIG_BACKUP_WIFI_PASSWORD_3);

    if (nmea_hdl) {
        nmea_parser_set_record_handler(nmea_hdl, on_record);
    }

    s_started = true;

    if (xTaskCreate(backup_task, "backup", 8192, NULL, 3, NULL) != pdTRUE) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "host=%s port=%u dwell=%d ms ignición=%s lab_gprs=%s redes=%u",
             s_host, (unsigned)s_port, CONFIG_BACKUP_DWELL_MS,
#if CONFIG_BACKUP_REQUIRE_IGNITION
             "exigida",
#else
             "no exigida",
#endif
#if CONFIG_BACKUP_LAB_ASSUME_GPRS_DOWN
             "asumir caído",
#else
             "real",
#endif
             (unsigned)conn_network_count());
    return ESP_OK;
}

#endif /* CONFIG_BACKUP_ENABLE */
