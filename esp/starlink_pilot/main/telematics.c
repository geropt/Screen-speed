#include "telematics.h"

#include "nmea_parser.h"
#include "ruptela_io_parser.h"
#include "ruptela_proto.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

static const char *TAG = "telematics";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_READY_BIT BIT2
#define REC_QUEUE_LEN 16
#define HEARTBEAT_MS 60000
#define GROUP_WAIT_MS 500
#define RECV_TIMEOUT_MS 200
#define WIFI_AP_MAX 3
#define CYCLE_MAX_RECORDS 8

typedef struct {
    const char *ssid;
    const char *password;
} wifi_ap_t;

typedef struct {
    uint8_t rec[RUPTELA_IO_MAX_RECORD_SIZE];
    uint8_t len;
} rec_item_t;

static QueueHandle_t s_rec_q;
static EventGroupHandle_t s_wifi_eg;
static int s_sock = -1;
static bool s_wifi_started;
static bool s_wifi_got_ip;
static bool s_want_wifi;
static bool s_wifi_connecting;
static int s_wifi_retries;

static uint64_t s_imei;
static bool s_imei_known;
static bool s_gprs_known;
static bool s_gprs_up;
static bool s_ign_known;
static bool s_ign_on;
static TickType_t s_gprs_down_since;

static rec_item_t s_cycle[CYCLE_MAX_RECORDS];
static uint16_t s_cycle_mask;
static uint8_t s_cycle_count;
static uint32_t s_cycle_ts;
static TickType_t s_cycle_tick;
static TickType_t s_last_tx;

static void close_socket(void)
{
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
        ESP_LOGI(TAG, "TCP closed");
    }
}

static bool lease_ok(void)
{
    if (!s_imei_known || !s_gprs_known) {
        return false;
    }
    if (s_gprs_up) {
        return false;
    }
#if CONFIG_TELEM_REQUIRE_IGNITION
    if (!s_ign_known || !s_ign_on) {
        return false;
    }
#endif
    TickType_t waited = xTaskGetTickCount() - s_gprs_down_since;
    return waited >= pdMS_TO_TICKS(CONFIG_TELEM_T_DOWN_MS);
}

static void log_queued_record(const uint8_t *rec, size_t len)
{
    if (len < 29U) {
        ESP_LOGI(TAG, "IO record queued len=%u", (unsigned)len);
        return;
    }

    size_t pos = 25U;
    static const uint8_t value_sizes[] = {1U, 2U, 4U, 8U};
    char ids[96];
    size_t n = 0;
    ids[0] = '\0';

    for (size_t group = 0; group < sizeof(value_sizes) && pos < len; ++group) {
        const uint8_t count = rec[pos++];
        const size_t item_size = 2U + value_sizes[group];
        for (uint8_t item = 0; item < count; ++item) {
            if (pos + item_size > len) {
                ESP_LOGI(TAG, "IO record queued len=%u ext=0x%02x (no IO walk)",
                         (unsigned)len, rec[5]);
                return;
            }
            const uint16_t io_id = (uint16_t)(((uint16_t)rec[pos] << 8) | rec[pos + 1]);
            if (n < sizeof(ids) - 8) {
                n += (size_t)snprintf(ids + n, sizeof(ids) - n, "%s%u", n ? "," : "", io_id);
            }
            pos += item_size;
        }
    }
    ESP_LOGI(TAG, "IO record queued len=%u ext=0x%02x ids=%s leftover=%u",
             (unsigned)len, rec[5], ids, (unsigned)(len - pos));
}

void telematics_on_nmea(void *event_handler_arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    (void)event_handler_arg;
    (void)event_base;

    switch (event_id) {
    case GPS_UPDATE: {
        const gps_t *gps = event_data;
        ESP_LOGI(TAG, "NMEA %s lat=%.5f lon=%.5f spd=%.1f",
                 gps->valid ? "fix" : "no-fix", gps->latitude, gps->longitude,
                 gps->speed * 3.6f);
        break;
    }
    case IGNITION_UPDATE: {
        const ignition_update_t *ign = event_data;
        s_ign_on = ign->ignition_on;
        s_ign_known = true;
        ESP_LOGI(TAG, "IO 409 ignition=%d", (int)s_ign_on);
        break;
    }
    case GPRS_UPDATE: {
        const gprs_update_t *g = event_data;
        if (!g->gprs_up) {
            if (!s_gprs_known || s_gprs_up) {
                s_gprs_down_since = xTaskGetTickCount();
            }
        } else if (!s_gprs_up) {
            ESP_LOGI(TAG, "IO 418 GPRS up: drop Starlink lease");
        }
        s_gprs_up = g->gprs_up;
        s_gprs_known = true;
        ESP_LOGI(TAG, "IO 418 gprs=%d", (int)s_gprs_up);
        break;
    }
    case IMEI_UPDATE: {
        const imei_update_t *m = event_data;
        if (!s_imei_known || s_imei != m->imei) {
            ESP_LOGI(TAG, "IMEI %" PRIu64, m->imei);
        }
        s_imei = m->imei;
        s_imei_known = true;
        break;
    }
    case RECORD_UPDATE: {
        const ruptela_record_update_t *r = event_data;
        rec_item_t item = {0};
        if (r->record_len == 0) {
            break;
        }
        memcpy(item.rec, r->record, r->record_len);
        item.len = r->record_len;
        if (!ruptela_io_record_can_tx(item.rec, item.len)) {
            ESP_LOGD(TAG, "drop non-record len=%u", (unsigned)item.len);
            break;
        }
        log_queued_record(item.rec, item.len);
        if (xQueueSend(s_rec_q, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "record queue full, dropping");
        }
        break;
    }
    default:
        break;
    }
}

static const wifi_ap_t s_wifi_aps[WIFI_AP_MAX] = {
    { CONFIG_TELEM_WIFI_SSID, CONFIG_TELEM_WIFI_PASSWORD },
    { CONFIG_TELEM_WIFI_SSID_2, CONFIG_TELEM_WIFI_PASSWORD_2 },
    { CONFIG_TELEM_WIFI_SSID_3, CONFIG_TELEM_WIFI_PASSWORD_3 },
};

static void wifi_apply_ap(int idx)
{
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_wifi_aps[idx].ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_aps[idx].password,
            sizeof(wifi_config.sta.password) - 1);
    if (s_wifi_aps[idx].password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "WiFi STA ssid=%s", s_wifi_aps[idx].ssid);
}

static int wifi_select_ap(void)
{
    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    uint16_t n = 20;
    wifi_ap_record_t recs[20];

    if (esp_wifi_scan_start(&scan, true) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (int i = 0; i < WIFI_AP_MAX; i++) {
            if (s_wifi_aps[i].ssid[0] == '\0') {
                continue;
            }
            for (uint16_t j = 0; j < n; j++) {
                if (strcmp((char *)recs[j].ssid, s_wifi_aps[i].ssid) == 0) {
                    ESP_LOGI(TAG, "scan hit ssid=%s rssi=%d", s_wifi_aps[i].ssid,
                             recs[j].rssi);
                    return i;
                }
            }
        }
        ESP_LOGW(TAG, "scan: configured SSIDs not in range (2.4 GHz)");
    }

    for (int i = 0; i < WIFI_AP_MAX; i++) {
        if (s_wifi_aps[i].ssid[0] != '\0') {
            return i;
        }
    }
    return -1;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_eg, WIFI_READY_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        ESP_LOGW(TAG, "WiFi disconnected retries=%d want=%d", s_wifi_retries,
                 (int)s_want_wifi);
        if (!s_want_wifi || !s_wifi_connecting) {
            if (!s_want_wifi) {
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            }
            return;
        }
        if (s_wifi_retries < 8) {
            s_wifi_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_wifi_retries = 0;
        s_wifi_got_ip = true;
        ESP_LOGI(TAG, "WiFi got IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_ensure(void)
{
    s_want_wifi = true;
    if (s_wifi_got_ip) {
        return ESP_OK;
    }
    s_wifi_connecting = false;
    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
        EventBits_t ready = xEventGroupWaitBits(s_wifi_eg, WIFI_READY_BIT, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(5000));
        if (!(ready & WIFI_READY_BIT)) {
            ESP_LOGW(TAG, "WiFi STA start timeout");
            return ESP_FAIL;
        }
    }

    int idx = wifi_select_ap();
    if (idx < 0) {
        ESP_LOGW(TAG, "no WiFi SSID configured");
        return ESP_FAIL;
    }
    if (s_wifi_aps[idx].password[0] == '\0') {
        ESP_LOGW(TAG, "ssid=%s has empty password; set it in menuconfig",
                 s_wifi_aps[idx].ssid);
    }
    s_wifi_retries = 0;
    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    wifi_apply_ap(idx);
    s_wifi_connecting = true;
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void wifi_stop_if_started(void)
{
    s_want_wifi = false;
    s_wifi_connecting = false;
    close_socket();
    if (s_wifi_started) {
        esp_wifi_disconnect();
        s_wifi_got_ip = false;
        s_wifi_retries = 0;
    }
}

static bool tcp_ensure(void)
{
    if (s_sock >= 0) {
        return true;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", CONFIG_TELEM_PORT);
    int err = getaddrinfo(CONFIG_TELEM_HOST, port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGW(TAG, "DNS %s failed: %d", CONFIG_TELEM_HOST, err);
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
        ESP_LOGW(TAG, "connect %s:%d failed errno=%d", CONFIG_TELEM_HOST,
                 CONFIG_TELEM_PORT, errno);
        close(sock);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    s_sock = sock;
    s_last_tx = xTaskGetTickCount();
    ESP_LOGI(TAG, "TCP connected %s:%d IMEI=%" PRIu64, CONFIG_TELEM_HOST,
             CONFIG_TELEM_PORT, s_imei);
    return true;
}

static bool send_all(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(s_sock, buf + off, len - off, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "send failed errno=%d", errno);
            close_socket();
            return false;
        }
        off += (size_t)n;
    }
    s_last_tx = xTaskGetTickCount();
    return true;
}

static uint32_t rec_ts(const rec_item_t *r)
{
    if (r->len < 4) {
        return 0;
    }
    return ((uint32_t)r->rec[0] << 24) | ((uint32_t)r->rec[1] << 16) |
           ((uint32_t)r->rec[2] << 8) | r->rec[3];
}

static uint8_t rec_ext(const rec_item_t *r)
{
    return (r->len > 5) ? r->rec[5] : 0;
}

static size_t build_records_packet(uint8_t *out, size_t cap,
                                   const rec_item_t *a, const rec_item_t *b)
{
    const uint8_t *records[2] = { a->rec, NULL };
    size_t record_lens[2] = { a->len, 0 };
    size_t count = 1;

    if (b != NULL) {
        records[1] = b->rec;
        record_lens[1] = b->len;
        count = 2;
    }

    return ruptela_build_cmd68(out, cap, s_imei, records, record_lens, count, 0);
}

static bool send_records(const rec_item_t *a, const rec_item_t *b)
{
    uint8_t pkt[RUPTELA_MAX_PACKET];
    size_t pkt_len = build_records_packet(pkt, sizeof(pkt), a, b);
    if (pkt_len == 0) {
        ESP_LOGW(TAG, "cmd68 build failed");
        return false;
    }
    ESP_LOGI(TAG, "TX cmd68 %u rec ext=0x%02x%s %u B",
             b != NULL ? 2U : 1U,
             rec_ext(a), b != NULL ? "+next" : "", (unsigned)pkt_len);
    return send_all(pkt, pkt_len);
}

static void reset_cycle(void)
{
    s_cycle_mask = 0;
    s_cycle_count = 0;
    s_cycle_ts = 0;
}

static bool flush_cycle(void)
{
    if (s_cycle_mask == 0) {
        return true;
    }

    const uint8_t *records[CYCLE_MAX_RECORDS];
    size_t record_lens[CYCLE_MAX_RECORDS];
    size_t count = 0;
    for (uint8_t i = 0; i < s_cycle_count; ++i) {
        if ((s_cycle_mask & (1U << i)) != 0) {
            records[count] = s_cycle[i].rec;
            record_lens[count] = s_cycle[i].len;
            ++count;
        }
    }

    uint8_t pkt[RUPTELA_MAX_PACKET];
    size_t pkt_len = ruptela_build_cmd68(pkt, sizeof(pkt), s_imei, records,
                                         record_lens, count, 0);
    reset_cycle();
    if (pkt_len == 0) {
        ESP_LOGW(TAG, "cmd68 cycle build failed");
        return false;
    }
    ESP_LOGI(TAG, "TX cmd68 cycle %u rec %u B", (unsigned)count,
             (unsigned)pkt_len);
    return send_all(pkt, pkt_len);
}

static bool handle_record(const rec_item_t *item)
{
    if (!ruptela_io_record_can_tx(item->rec, item->len)) {
        return true;
    }
    uint8_t ext = rec_ext(item);
    uint8_t last_index = (uint8_t)(ext >> 4);
    uint8_t idx = (uint8_t)(ext & 0x0FU);

    if (last_index == 0U && idx == 0U) {
        if (!flush_cycle()) {
            return false;
        }
        return send_records(item, NULL);
    }

    uint8_t count = (uint8_t)(last_index + 1U);
    if (count > CYCLE_MAX_RECORDS || idx >= count) {
        if (!flush_cycle()) {
            return false;
        }
        return send_records(item, NULL);
    }

    uint32_t timestamp = rec_ts(item);
    uint16_t bit = (uint16_t)(1U << idx);
    if (s_cycle_mask != 0 &&
        (s_cycle_ts != timestamp || s_cycle_count != count ||
         (s_cycle_mask & bit) != 0)) {
        if (!flush_cycle()) {
            return false;
        }
    }

    if (s_cycle_mask == 0) {
        s_cycle_ts = timestamp;
        s_cycle_count = count;
        s_cycle_tick = xTaskGetTickCount();
    }
    s_cycle[idx] = *item;
    s_cycle_mask |= bit;

    uint16_t complete_mask = (uint16_t)((1U << count) - 1U);
    return s_cycle_mask == complete_mask ? flush_cycle() : true;
}

static void handle_server_bytes(uint8_t *buf, size_t *filled)
{
    size_t off = 0;
    while (off + 5 <= *filled) {
        ruptela_server_msg_t msg;
        size_t used = ruptela_parse_server_packet(buf + off, *filled - off, &msg);
        if (used == 0) {
            if (*filled - off > 4 && (*filled - off) >= (size_t)(2 + (buf[off] << 8 | buf[off + 1]) + 2)) {
                ESP_LOGW(TAG, "bad server CRC, drop 1");
                off += 1;
                continue;
            }
            break;
        }
        ESP_LOGI(TAG, "RX cmd %u payload %u", msg.cmd, (unsigned)msg.payload_len);
        if (ruptela_cmd_must_drop(msg.cmd)) {
            ESP_LOGW(TAG, "server cmd %u is not for the HUD, closing without ACK", msg.cmd);
            close_socket();
            *filled = 0;
            return;
        }
        off += used;
    }
    if (off > 0 && off < *filled) {
        memmove(buf, buf + off, *filled - off);
        *filled -= off;
    } else if (off >= *filled) {
        *filled = 0;
    }
}

static void recv_some(void)
{
    static uint8_t rx[1024];
    static size_t filled;

    if (s_sock < 0) {
        filled = 0;
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = RECV_TIMEOUT_MS * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int n = recv(s_sock, rx + filled, sizeof(rx) - filled, 0);
    if (n > 0) {
        filled += (size_t)n;
        handle_server_bytes(rx, &filled);
    } else if (n == 0) {
        close_socket();
        filled = 0;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        close_socket();
        filled = 0;
    }
}

static bool send_heartbeat(void)
{
    uint8_t pkt[16];
    size_t n = ruptela_build_heartbeat(pkt, sizeof(pkt), s_imei);
    if (n == 0) {
        return false;
    }
    ESP_LOGI(TAG, "TX heartbeat 16");
    return send_all(pkt, n);
}

static void telematics_task(void *arg)
{
    (void)arg;
    rec_item_t item;
    while (1) {
        if (!lease_ok()) {
            reset_cycle();
            if (s_sock >= 0 || s_wifi_got_ip) {
                wifi_stop_if_started();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (wifi_ensure() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!tcp_ensure()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (s_cycle_mask != 0 &&
            (xTaskGetTickCount() - s_cycle_tick) >= pdMS_TO_TICKS(GROUP_WAIT_MS)) {
            flush_cycle();
        }

        while (xQueueReceive(s_rec_q, &item, 0) == pdTRUE) {
            if (!handle_record(&item)) {
                break;
            }
        }

        recv_some();

        if (s_sock >= 0 &&
            (xTaskGetTickCount() - s_last_tx) >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
            send_heartbeat();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void telematics_start(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_eg = xEventGroupCreate();
    s_rec_q = xQueueCreate(REC_QUEUE_LEN, sizeof(rec_item_t));
    xTaskCreate(telematics_task, "telematics", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "host=%s port=%d t_down=%d ms", CONFIG_TELEM_HOST,
             CONFIG_TELEM_PORT, CONFIG_TELEM_T_DOWN_MS);
}
