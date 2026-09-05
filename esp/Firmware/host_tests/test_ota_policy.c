/* Pruebas de la política de OTA.
 *
 * Los dos casos que el plan nombra por su cuenta y que están acá:
 *  - «ausencia de Internet/GPS/SD no equivale a imagen defectuosa»;
 *  - «un segundo OTA no pisa el rollback antes de que el anterior haya sido
 *    confirmado».
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_policy.h"

static const char MODEL[] = "waveshare-amoled-175";

static void fill_hash(uint8_t *h, uint8_t seed)
{
    for (size_t i = 0; i < OTA_HASH_LEN; ++i) {
        h[i] = (uint8_t)(seed + i);
    }
}

static ota_device_ctx_t device(void)
{
    ota_device_ctx_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.model, MODEL, OTA_MODEL_MAX - 1);
    d.running_version = 10;
    d.min_allowed_version = 5;
    d.slot_capacity = 3 * 1024 * 1024;
    d.nvs_schema = 2;
    d.map_container_version = 1;
    d.require_signature = true;
    d.power_ok = true;
    return d;
}

static ota_image_desc_t image(uint32_t version, uint8_t hash_seed)
{
    ota_image_desc_t i;
    memset(&i, 0, sizeof(i));
    strncpy(i.model, MODEL, OTA_MODEL_MAX - 1);
    i.version = version;
    i.size_bytes = 1700000;
    fill_hash(i.hash, hash_seed);
    i.signature_present = true;
    i.signature_valid = true;
    i.nvs_schema_min = 1;
    i.nvs_schema_max = 3;
    i.map_container_min = 1;
    i.map_container_max = 1;
    return i;
}

/* Autoprueba que pasa. */
static ota_selftest_obs_t good_obs(void)
{
    ota_selftest_obs_t o;
    memset(&o, 0, sizeof(o));
    o.speed_displayed = true;
    o.uptime_ms = OTA_SELFTEST_WINDOW_MS + 1000;
    return o;
}

/* ---------- verificación ---------- */

static void test_camino_feliz(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);

    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
    assert(ota_policy_select_boot(&p) == OTA_OK);
    assert(ota_policy_rollback_pending(&p) == true);

    ota_selftest_obs_t obs = good_obs();
    bool rollback = true;
    assert(ota_policy_evaluate_selftest(&p, &obs, &rollback) == OTA_OK);
    assert(rollback == false);
    assert(ota_policy_confirm(&p) == OTA_OK);
    assert(ota_policy_rollback_pending(&p) == false);
    /* La que corría pasa a ser la candidata del próximo OTA. */
    assert(p.running_slot == OTA_SLOT_B);
    assert(p.candidate_slot == OTA_SLOT_A);
}

static void test_imagen_de_otra_placa(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    strncpy(img.model, "otra-placa", OTA_MODEL_MAX - 1);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);

    /* El error más caro: deja el equipo sin arrancar. Se rechaza primero. */
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_MODEL);
    /* Y sin verificar no hay camino a seleccionar el boot. */
    assert(ota_policy_select_boot(&p) == OTA_ERR_STATE);
}

static void test_imagen_que_no_cabe(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    img.size_bytes = d.slot_capacity + 1;
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_OVERSIZE);

    img.size_bytes = 0;
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_OVERSIZE);
}

static void test_hash_que_no_coincide(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xBB);   /* lo calculado no coincide con lo declarado */
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_HASH);
}

static void test_firma_ausente_o_invalida(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);

    ota_image_desc_t img = image(11, 0xAA);
    img.signature_present = false;
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_SIGNATURE);

    img.signature_present = true;
    img.signature_valid = false;
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_SIGNATURE);

    /* Con la exigencia de firma apagada —unidad de desarrollo— la misma imagen pasa.
     * Que sea configurable es deliberado; que el default sea exigirla, también. */
    d.require_signature = false;
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
}

static void test_anti_rollback(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();   /* min_allowed = 5 */
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);

    ota_image_desc_t old_img = image(4, 0xAA);
    assert(ota_policy_verify(&p, &old_img, &d, h) == OTA_ERR_ANTI_ROLLBACK);

    /* Exactamente el mínimo sí se permite. */
    ota_image_desc_t at_min = image(5, 0xAA);
    assert(ota_policy_verify(&p, &at_min, &d, h) == OTA_OK);
}

static void test_anti_rollback_inhabilita_el_fallback(void)
{
    /* Las dos caras de la misma decisión, que hay que ver juntas. */
    ota_device_ctx_t d = device();   /* min_allowed = 5 */
    assert(ota_policy_fallback_allowed(&d, 7) == true);
    assert(ota_policy_fallback_allowed(&d, 5) == true);
    /* Volver a la 4 no está permitido: subir el mínimo dejó sin fallback a las
     * unidades cuya imagen anterior es la 4. */
    assert(ota_policy_fallback_allowed(&d, 4) == false);
    assert(ota_policy_fallback_allowed(NULL, 9) == false);
}

static void test_misma_version(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();   /* corriendo la 10 */
    ota_image_desc_t img = image(10, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_SAME_VERSION);
}

static void test_incompatibilidad_de_nvs(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();   /* nvs_schema = 2 */
    ota_image_desc_t img = image(11, 0xAA);
    img.nvs_schema_min = 5;
    img.nvs_schema_max = 9;
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    /* Si la imagen nueva no puede leer la configuración instalada, volver atrás no
     * alcanzaría para recuperar el equipo. */
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_NVS_INCOMPAT);
}

static void test_incompatibilidad_de_mapas(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();   /* contenedor de mapas v1 */
    ota_image_desc_t img = image(11, 0xAA);
    img.map_container_min = 2;
    img.map_container_max = 3;
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_MAP_INCOMPAT);

    /* Sin mapas instalados la compatibilidad de mapas no aplica. */
    d.map_container_version = 0;
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
}

static void test_energia_insuficiente(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    d.power_ok = false;
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_ERR_POWER);
}

/* ---------- autoprueba ---------- */

static void test_ausencia_de_servicios_no_es_falla_de_imagen(void)
{
    /* EL caso que el plan nombra: sin Internet, sin fix de GPS y sin tarjeta, la
     * imagen se confirma igual. El HUD tiene que funcionar sin las tres, y tomarlas
     * como falla produciría rollbacks por estar en un estacionamiento subterráneo. */
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
    assert(ota_policy_select_boot(&p) == OTA_OK);

    ota_selftest_obs_t obs = good_obs();
    obs.no_internet = true;
    obs.no_gps_fix = true;
    obs.no_sd_card = true;
    bool rollback = true;
    assert(ota_policy_evaluate_selftest(&p, &obs, &rollback) == OTA_OK);
    assert(rollback == false);
    assert(ota_policy_confirm(&p) == OTA_OK);
}

static void test_fallas_internas_disparan_rollback(void)
{
    struct { const char *what; ota_selftest_obs_t obs; } cases[4];
    for (int i = 0; i < 4; ++i) {
        cases[i].obs = good_obs();
    }
    cases[0].what = "panic"; cases[0].obs.panic_or_watchdog = true;
    cases[1].what = "display"; cases[1].obs.display_init_failed = true;
    cases[2].what = "tareas"; cases[2].obs.task_create_failed = true;
    cases[3].what = "uart"; cases[3].obs.uart_init_failed = true;

    for (int i = 0; i < 4; ++i) {
        ota_policy_t p;
        ota_policy_init(&p, OTA_SLOT_A, 0);
        ota_device_ctx_t d = device();
        ota_image_desc_t img = image(11, 0xAA);
        uint8_t h[OTA_HASH_LEN];
        fill_hash(h, 0xAA);
        assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
        assert(ota_policy_select_boot(&p) == OTA_OK);

        bool rollback = false;
        ota_policy_evaluate_selftest(&p, &cases[i].obs, &rollback);
        /* Lo interno sí cuenta: la imagen no funciona. */
        assert(rollback == true);
        assert(ota_policy_confirm(&p) == OTA_OK ||
               ota_policy_mark_rolled_back(&p) == OTA_OK);
    }
}

static void test_no_decide_antes_de_la_ventana(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
    assert(ota_policy_select_boot(&p) == OTA_OK);

    ota_selftest_obs_t obs = good_obs();
    obs.uptime_ms = 1000;    /* la ventana es de 60 s */
    bool rollback = true;
    /* Todavía no corresponde decidir, y sobre todo NO corresponde volver atrás. */
    assert(ota_policy_evaluate_selftest(&p, &obs, &rollback) == OTA_ERR_STATE);
    assert(rollback == false);
}

static void test_sin_velocidad_en_la_ventana_vuelve(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    ota_image_desc_t img = image(11, 0xAA);
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    assert(ota_policy_verify(&p, &img, &d, h) == OTA_OK);
    assert(ota_policy_select_boot(&p) == OTA_OK);

    ota_selftest_obs_t obs = good_obs();
    obs.speed_displayed = false;
    bool rollback = false;
    ota_policy_evaluate_selftest(&p, &obs, &rollback);
    assert(rollback == true);
    assert(ota_policy_mark_rolled_back(&p) == OTA_OK);
    /* Después de volver, el slot que corría sigue siendo el mismo. */
    assert(p.running_slot == OTA_SLOT_A);
    assert(ota_policy_rollback_pending(&p) == false);
}

/* ---------- un segundo OTA no pisa el rollback ---------- */

static void test_segundo_ota_no_pisa_el_rollback(void)
{
    /* El otro caso que el plan nombra. */
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);

    ota_image_desc_t first = image(11, 0xAA);
    assert(ota_policy_verify(&p, &first, &d, h) == OTA_OK);
    assert(ota_policy_select_boot(&p) == OTA_OK);
    assert(ota_policy_rollback_pending(&p) == true);

    /* Llega otra imagen antes de confirmar la primera: si esta también falla, ya no
     * habría a dónde volver. */
    ota_image_desc_t second = image(12, 0xAA);
    assert(ota_policy_verify(&p, &second, &d, h) == OTA_ERR_UNCONFIRMED);

    /* Después de confirmar, sí se acepta. */
    ota_selftest_obs_t obs = good_obs();
    bool rollback = true;
    assert(ota_policy_evaluate_selftest(&p, &obs, &rollback) == OTA_OK);
    assert(ota_policy_confirm(&p) == OTA_OK);
    d.running_version = 11;
    assert(ota_policy_verify(&p, &second, &d, h) == OTA_OK);
}

static void test_no_se_acumulan_candidatas(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    ota_device_ctx_t d = device();
    uint8_t h[OTA_HASH_LEN];
    fill_hash(h, 0xAA);
    ota_image_desc_t a = image(11, 0xAA);
    assert(ota_policy_verify(&p, &a, &d, h) == OTA_OK);
    ota_image_desc_t b = image(12, 0xAA);
    assert(ota_policy_verify(&p, &b, &d, h) == OTA_ERR_STATE);
}

static void test_transiciones_invalidas(void)
{
    ota_policy_t p;
    ota_policy_init(&p, OTA_SLOT_A, 0);
    assert(ota_policy_confirm(&p) == OTA_ERR_STATE);
    assert(ota_policy_mark_rolled_back(&p) == OTA_ERR_STATE);
    assert(ota_policy_select_boot(&p) == OTA_ERR_STATE);

    ota_selftest_obs_t obs = good_obs();
    bool rollback = false;
    assert(ota_policy_evaluate_selftest(&p, &obs, &rollback) == OTA_ERR_STATE);
}

static void test_punteros_nulos(void)
{
    ota_policy_init(NULL, OTA_SLOT_A, 0);
    assert(ota_policy_verify(NULL, NULL, NULL, NULL) == OTA_ERR_ARG);
    assert(ota_policy_select_boot(NULL) == OTA_ERR_ARG);
    assert(ota_policy_evaluate_selftest(NULL, NULL, NULL) == OTA_ERR_ARG);
    assert(ota_policy_confirm(NULL) == OTA_ERR_ARG);
    assert(ota_policy_mark_rolled_back(NULL) == OTA_ERR_ARG);
    assert(ota_policy_rollback_pending(NULL) == false);
    assert(strcmp(ota_err_name(OTA_ERR_MODEL), "modelo_ajeno") == 0);
    assert(strcmp(ota_state_name(OTA_STATE_PENDING_CONFIRM), "sin_confirmar") == 0);
}

int main(void)
{
    test_camino_feliz();
    test_imagen_de_otra_placa();
    test_imagen_que_no_cabe();
    test_hash_que_no_coincide();
    test_firma_ausente_o_invalida();
    test_anti_rollback();
    test_anti_rollback_inhabilita_el_fallback();
    test_misma_version();
    test_incompatibilidad_de_nvs();
    test_incompatibilidad_de_mapas();
    test_energia_insuficiente();
    test_ausencia_de_servicios_no_es_falla_de_imagen();
    test_fallas_internas_disparan_rollback();
    test_no_decide_antes_de_la_ventana();
    test_sin_velocidad_en_la_ventana_vuelve();
    test_segundo_ota_no_pisa_el_rollback();
    test_no_se_acumulan_candidatas();
    test_transiciones_invalidas();
    test_punteros_nulos();
    printf("ota_policy tests passed\n");
    return 0;
}
