#include "map_sync.h"
#include "net_link.h"
#include "sd_manager.h"   // MOUNT_POINT

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

static const char *TAG = "MAP_SYNC";

// NVS: estado de mapas (desacoplado de "netcfg" que tiene las credenciales).
#define MAP_NVS_NS        "mapcfg"
#define MAP_NVS_K_SEL     "sel_zones"   // CSV de ids de zonas elegidas
#define MAP_NVS_K_UPD     "upd_flag"    // u8: hay mapas nuevos sin avisar
#define MAP_SEL_MAX       16            // tope de zonas elegibles
#define MAP_SEL_CSV_LEN   (MAP_SEL_MAX * sizeof(((map_zone_t *)0)->id))

#define TILES_DIR         MOUNT_POINT "/tiles"
#define DL_CHUNK          1024          // chunk de descarga / hashing

// Topes de respuesta para acotar el heap (el catalogo/manifest son JSON chicos).
#define CATALOG_MAX_BYTES   (16 * 1024)
#define MANIFEST_MAX_BYTES  (64 * 1024)

// Timeout de red por request.
#define HTTP_TIMEOUT_MS     15000

#if CONFIG_MAP_SYNC_TEST_CERT
extern const char test_server_cert_pem_start[] asm("_binary_test_server_cert_pem_start");
#endif

// ---------------------------------------------------------------------------
// Construye "<base_url>/<path>" (path sin barra inicial). Devuelve false si no
// hay base_url o no entra en out.
// ---------------------------------------------------------------------------
static bool build_url(const char *path, char *out, size_t out_len)
{
    const char *base = net_link_get_base_url();
    if (base == NULL || base[0] == '\0') {
        ESP_LOGW(TAG, "sin base_url en NVS");
        return false;
    }
    size_t blen = strlen(base);
    bool slash = (blen > 0 && base[blen - 1] == '/');
    int n = snprintf(out, out_len, "%s%s%s", base, slash ? "" : "/", path);
    return (n > 0 && (size_t)n < out_len);
}

// ---------------------------------------------------------------------------
// GET HTTPS a *url*. En exito asigna *body (NUL-terminado, liberar con free) y
// escribe el largo en *len. cap acota el tamano maximo aceptado.
// ---------------------------------------------------------------------------
// Inicializa el cliente HTTP con la verificacion TLS adecuada (bundle de prod o
// cert self-signed embebido para el server de prueba).
static esp_http_client_handle_t client_init(const char *url)
{
    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
#if CONFIG_MAP_SYNC_TEST_CERT
        .cert_pem       = test_server_cert_pem_start,   // self-signed embebido
#else
        .crt_bundle_attach = esp_crt_bundle_attach,     // produccion: bundle mbedTLS
#endif
    };
    return esp_http_client_init(&cfg);
}

static esp_err_t https_get(const char *url, char **body, size_t *len, size_t cap)
{
    *body = NULL;
    if (len) *len = 0;

    esp_http_client_handle_t client = client_init(url);
    if (client == NULL)
        return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    char *buf = NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s: %s", url, esp_err_to_name(err));
        goto done;
    }

    esp_http_client_fetch_headers(client);  // puede ser -1 (chunked); leemos igual
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "%s HTTP %d", url, status);
        goto done;
    }

    buf = malloc(cap);
    if (buf == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t total = 0;
    while (total < cap - 1) {
        int r = esp_http_client_read(client, buf + total, cap - 1 - total);
        if (r < 0) {
            ESP_LOGE(TAG, "read %s falló", url);
            goto done;
        }
        if (r == 0)
            break;  // fin del cuerpo
        total += r;
    }
    if (total >= cap - 1) {
        ESP_LOGE(TAG, "%s respuesta > %u bytes", url, (unsigned)cap);
        goto done;
    }
    buf[total] = '\0';

    *body = buf;
    buf = NULL;  // ownership transferido
    if (len) *len = total;
    ret = ESP_OK;

done:
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

// ---------------------------------------------------------------------------
// Catalogo
// ---------------------------------------------------------------------------
esp_err_t map_sync_fetch_catalog(map_zone_t **out, int *count)
{
    if (out) *out = NULL;
    if (count) *count = 0;

    char url[160];
    if (!build_url("catalog.json", url, sizeof(url)))
        return ESP_ERR_INVALID_STATE;

    char  *body = NULL;
    size_t blen = 0;
    esp_err_t err = https_get(url, &body, &blen, CATALOG_MAX_BYTES);
    if (err != ESP_OK)
        return err;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "catalog.json: JSON invalido");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(root, "zones");
    int n = cJSON_IsArray(zones) ? cJSON_GetArraySize(zones) : 0;
    if (n <= 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "catalog sin zonas");
        return ESP_ERR_INVALID_RESPONSE;
    }

    map_zone_t *arr = calloc(n, sizeof(map_zone_t));
    if (arr == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int k = 0;
    const cJSON *z;
    cJSON_ArrayForEach(z, zones) {
        const cJSON *jid   = cJSON_GetObjectItemCaseSensitive(z, "id");
        const cJSON *jname = cJSON_GetObjectItemCaseSensitive(z, "name");
        const cJSON *jver  = cJSON_GetObjectItemCaseSensitive(z, "version");
        const cJSON *jtil  = cJSON_GetObjectItemCaseSensitive(z, "tiles");
        const cJSON *jby   = cJSON_GetObjectItemCaseSensitive(z, "bytes");
        if (!cJSON_IsString(jid) || !jid->valuestring || !jid->valuestring[0])
            continue;  // zona sin id valido: la salteamos
        map_zone_t *e = &arr[k++];
        strlcpy(e->id, jid->valuestring, sizeof(e->id));
        if (cJSON_IsString(jname) && jname->valuestring)
            strlcpy(e->name, jname->valuestring, sizeof(e->name));
        e->version = cJSON_IsNumber(jver) ? jver->valueint : 0;
        e->tiles   = cJSON_IsNumber(jtil) ? jtil->valueint : 0;
        e->bytes   = cJSON_IsNumber(jby)  ? (size_t)jby->valuedouble : 0;
    }
    cJSON_Delete(root);

    if (k == 0) {
        free(arr);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out)   *out = arr;   else free(arr);
    if (count) *count = k;
    return ESP_OK;
}

void map_sync_free_catalog(map_zone_t *zones)
{
    free(zones);
}

// ---------------------------------------------------------------------------
// Zonas elegidas (NVS mapcfg/sel_zones, CSV)
// ---------------------------------------------------------------------------
esp_err_t map_sync_set_selected(const char *const *ids, int n)
{
    char csv[MAP_SEL_CSV_LEN];
    csv[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < n && i < MAP_SEL_MAX; i++) {
        if (ids[i] == NULL || ids[i][0] == '\0')
            continue;
        int w = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         off ? "," : "", ids[i]);
        if (w <= 0 || (size_t)w >= sizeof(csv) - off)
            break;  // no entra: cortamos
        off += w;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MAP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, MAP_NVS_K_SEL, csv);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "zonas elegidas: '%s'", csv);
    return err;
}

int map_sync_get_selected(char ids[][24], int max)
{
    char csv[MAP_SEL_CSV_LEN];
    size_t len = sizeof(csv);
    nvs_handle_t h;
    if (nvs_open(MAP_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return 0;
    esp_err_t err = nvs_get_str(h, MAP_NVS_K_SEL, csv, &len);
    nvs_close(h);
    if (err != ESP_OK || csv[0] == '\0')
        return 0;

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(csv, ",", &save);
         tok != NULL && n < max;
         tok = strtok_r(NULL, ",", &save)) {
        strlcpy(ids[n++], tok, 24);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Flag "mapas actualizados" (NVS)
// ---------------------------------------------------------------------------
static void set_upd_flag(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(MAP_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, MAP_NVS_K_UPD, v);
    nvs_commit(h);
    nvs_close(h);
}

bool map_sync_updates_pending(void)
{
    nvs_handle_t h;
    if (nvs_open(MAP_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    uint8_t v = 0;
    nvs_get_u8(h, MAP_NVS_K_UPD, &v);
    nvs_close(h);
    return v != 0;
}

void map_sync_clear_updates_flag(void)
{
    set_upd_flag(0);
}

// ---------------------------------------------------------------------------
// Descarga atomica de un tile con verificacion sha256
// ---------------------------------------------------------------------------
static void sha_to_hex(const uint8_t *d, char *hex)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i]     = H[d[i] >> 4];
        hex[2 * i + 1] = H[d[i] & 0xf];
    }
    hex[64] = '\0';
}

// Baja <base_url>/<rel_url> a "<dest>.tmp", verifica sha256 y hace rename atomico.
static esp_err_t download_tile(const char *rel_url, const char *dest, const char *want_sha)
{
    char url[200];
    if (!build_url(rel_url, url, sizeof(url)))
        return ESP_ERR_INVALID_STATE;

    char tmp[170];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);

    esp_http_client_handle_t client = client_init(url);
    if (client == NULL)
        return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    FILE *f = NULL;
    uint8_t *buf = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (esp_http_client_open(client, 0) != ESP_OK)
        goto done;
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200)
        goto done;

    f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "no se pudo crear %s", tmp);
        goto done;
    }
    buf = malloc(DL_CHUNK);
    if (buf == NULL)
        goto done;

    mbedtls_sha256_starts(&sha, 0);
    for (;;) {
        int r = esp_http_client_read(client, (char *)buf, DL_CHUNK);
        if (r < 0)
            goto done;
        if (r == 0)
            break;
        mbedtls_sha256_update(&sha, buf, r);
        if (fwrite(buf, 1, r, f) != (size_t)r)
            goto done;
    }
    fclose(f);
    f = NULL;

    uint8_t dig[32];
    char hex[65];
    mbedtls_sha256_finish(&sha, dig);
    sha_to_hex(dig, hex);
    if (want_sha && strcasecmp(hex, want_sha) != 0) {
        ESP_LOGW(TAG, "sha256 no coincide en %s", dest);
        goto done;
    }
    if (rename(tmp, dest) != 0) {
        ESP_LOGE(TAG, "rename %s falló", dest);
        goto done;
    }
    ret = ESP_OK;

done:
    if (f) fclose(f);
    free(buf);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK)
        unlink(tmp);
    return ret;
}

// ---------------------------------------------------------------------------
// Sincronizacion: delta + descarga de las zonas elegidas
// ---------------------------------------------------------------------------
esp_err_t map_sync_run(map_sync_result_t *out)
{
    map_sync_result_t res = {0};

    char ids[MAP_SEL_MAX][24];
    int nz = map_sync_get_selected(ids, MAP_SEL_MAX);
    if (nz == 0) {
        ESP_LOGI(TAG, "sin zonas elegidas; nada que sincronizar");
        if (out) *out = res;
        return ESP_OK;
    }

    mkdir(TILES_DIR, 0777);   // idempotente; si ya existe, no pasa nada

    esp_err_t final = ESP_OK;
    for (int z = 0; z < nz; z++) {
        char rel[48];
        // %.23s: el id de zona nunca supera 23 chars (array de 24); acota la
        // longitud para -Wformat-truncation (GCC no sabe que esta NUL-terminado).
        snprintf(rel, sizeof(rel), "zones/%.23s.json", ids[z]);
        char manurl[200];
        if (!build_url(rel, manurl, sizeof(manurl))) {
            final = ESP_FAIL;
            continue;
        }

        char *body = NULL;
        size_t blen = 0;
        if (https_get(manurl, &body, &blen, MANIFEST_MAX_BYTES) != ESP_OK) {
            ESP_LOGW(TAG, "manifiesto de '%s' no bajo", ids[z]);
            final = ESP_FAIL;
            continue;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root == NULL) {
            final = ESP_FAIL;
            continue;
        }
        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(root, "tiles");
        const cJSON *t;
        cJSON_ArrayForEach(t, tiles) {
            const cJSON *jname = cJSON_GetObjectItemCaseSensitive(t, "name");
            const cJSON *jsize = cJSON_GetObjectItemCaseSensitive(t, "size");
            const cJSON *jsha  = cJSON_GetObjectItemCaseSensitive(t, "sha256");
            const cJSON *jurl  = cJSON_GetObjectItemCaseSensitive(t, "url");
            if (!cJSON_IsString(jname) || !jname->valuestring ||
                !cJSON_IsString(jurl)  || !jurl->valuestring)
                continue;

            res.checked++;
            char dest[160];
            snprintf(dest, sizeof(dest), "%s/%s", TILES_DIR, jname->valuestring);

            // delta: bajar si falta o si el size local difiere del manifiesto
            struct stat st;
            long want_size = cJSON_IsNumber(jsize) ? (long)jsize->valuedouble : -1;
            bool need = (stat(dest, &st) != 0) ||
                        (want_size >= 0 && st.st_size != want_size);
            if (!need)
                continue;

            res.changed++;
            const char *sha = cJSON_IsString(jsha) ? jsha->valuestring : NULL;
            if (download_tile(jurl->valuestring, dest, sha) == ESP_OK)
                res.downloaded++;
            else
                res.failed++;
        }
        cJSON_Delete(root);
    }

    res.any_update = (res.downloaded > 0);
    if (res.any_update)
        set_upd_flag(1);

    ESP_LOGI(TAG, "sync: %d checked, %d changed, %d bajados, %d fallidos",
             res.checked, res.changed, res.downloaded, res.failed);
    if (res.failed > 0)
        final = ESP_FAIL;
    if (out) *out = res;
    return final;
}

// ---------------------------------------------------------------------------
// Selftest (Incremento 2): connect -> fetch catalog -> log -> stop
// ---------------------------------------------------------------------------
void map_sync_selftest(void)
{
#if CONFIG_MAP_SYNC_BOOT_SELFTEST
    ESP_LOGI(TAG, "selftest: conectando WiFi...");
    if (net_link_connect(20000) != ESP_OK) {
        ESP_LOGW(TAG, "selftest: WiFi no conecto");
        return;
    }

    map_zone_t *zones = NULL;
    int n = 0;
    esp_err_t err = map_sync_fetch_catalog(&zones, &n);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "selftest: catalogo OK, %d zonas:", n);
        for (int i = 0; i < n; i++)
            ESP_LOGI(TAG, "  [%s] '%s' v%d  %d tiles  %u B",
                     zones[i].id, zones[i].name, zones[i].version,
                     zones[i].tiles, (unsigned)zones[i].bytes);
        map_sync_free_catalog(zones);
    } else {
        ESP_LOGE(TAG, "selftest: fetch catalogo fallo (%s)", esp_err_to_name(err));
    }

    net_link_stop();
    ESP_LOGI(TAG, "selftest: WiFi detenido");
#endif
}
