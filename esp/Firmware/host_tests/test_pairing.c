/* Pruebas del pairing: posesión única, ventana, intentos, revocación y la ligadura
 * entre la sesión bulk y la vinculación.
 *
 * Lo central que se fija: **no existe un canal que acepte bytes sólo porque otro canal
 * autenticó**. El plan lo pide explícitamente y es el error clásico de este tipo de
 * diseño.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pairing.h"

static void fill_secret(uint8_t *s, uint8_t seed)
{
    for (size_t i = 0; i < PAIRING_SECRET_LEN; ++i) {
        s[i] = (uint8_t)(seed + i);
    }
}

static pairing_hello_t client_hello(uint32_t caps)
{
    pairing_hello_t h;
    h.api_version_min = 1;
    h.api_version_max = 1;
    h.capabilities = caps;
    return h;
}

static const uint32_t ALL_CAPS = PAIR_CAP_MAP_INSTALL | PAIR_CAP_OTA |
                                 PAIR_CAP_BULK_BLE | PAIR_CAP_BULK_WIFI |
                                 PAIR_CAP_DIAG_READ | PAIR_CAP_RESUME_SWITCH;

/* Vincula un cliente y devuelve el bond_id. */
static uint64_t bond(pairing_t *p, uint64_t now, uint8_t seed)
{
    uint8_t secret[PAIRING_SECRET_LEN];
    fill_secret(secret, seed);
    assert(pairing_open_window(p, now, secret) == PAIR_OK);
    pairing_hello_t cl = client_hello(ALL_CAPS);
    uint64_t id = 0;
    assert(pairing_complete(p, now + 1000, secret, &cl, &id) == PAIR_OK);
    return id;
}

/* ---------- negociación ---------- */

static void test_negociacion_elige_la_mas_alta_comun(void)
{
    pairing_hello_t dev = { 1, 3, PAIR_CAP_OTA | PAIR_CAP_BULK_BLE };
    pairing_hello_t cl  = { 2, 5, PAIR_CAP_OTA | PAIR_CAP_BULK_WIFI };
    pairing_negotiation_t n = pairing_negotiate(&dev, &cl);
    assert(n.ok == true);
    assert(n.api_version == 3);            /* la más alta que ambos soportan */
    /* Intersección: BULK_WIFI lo pide el cliente pero el dispositivo no lo declara. */
    assert(n.capabilities == PAIR_CAP_OTA);
}

static void test_negociacion_sin_version_comun(void)
{
    pairing_hello_t dev = { 1, 2, ALL_CAPS };
    pairing_hello_t cl  = { 5, 7, ALL_CAPS };
    pairing_negotiation_t n = pairing_negotiate(&dev, &cl);
    /* No se negocia una versión "parecida": interpretar un protocolo desconocido es
     * peor que no hablar. */
    assert(n.ok == false);
    assert(n.capabilities == 0);
}

static void test_cliente_mas_nuevo_degrada(void)
{
    pairing_hello_t dev = { 1, 1, ALL_CAPS };
    pairing_hello_t cl  = { 1, 9, ALL_CAPS };
    pairing_negotiation_t n = pairing_negotiate(&dev, &cl);
    assert(n.ok == true);
    assert(n.api_version == 1);
}

/* ---------- ventana e intentos ---------- */

static void test_vinculacion_completa(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    assert(pairing_state(&p) == PAIR_STATE_UNPAIRED);

    uint64_t id = bond(&p, 1000, 0x10);
    assert(id != 0);
    assert(pairing_state(&p) == PAIR_STATE_PAIRED);
    /* Sólo las capacidades que el dispositivo declara. */
    assert(pairing_has_capability(&p, PAIR_CAP_MAP_INSTALL) == true);
    assert(pairing_has_capability(&p, PAIR_CAP_OTA) == true);
    /* BULK_WIFI y RESUME_SWITCH no están implementadas, así que no se declaran. */
    assert(pairing_has_capability(&p, PAIR_CAP_BULK_WIFI) == false);
    assert(pairing_has_capability(&p, PAIR_CAP_RESUME_SWITCH) == false);
}

static void test_fuera_de_la_ventana_se_rechaza(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint8_t secret[PAIRING_SECRET_LEN];
    fill_secret(secret, 0x20);
    assert(pairing_open_window(&p, 1000, secret) == PAIR_OK);

    pairing_hello_t cl = client_hello(ALL_CAPS);
    uint64_t id = 0;
    /* Con el secreto correcto pero tarde: la ventana es parte de la autorización. */
    assert(pairing_complete(&p, 1000 + PAIRING_WINDOW_MS_DEFAULT + 1,
                           secret, &cl, &id) == PAIR_ERR_WINDOW_CLOSED);
    assert(pairing_state(&p) != PAIR_STATE_PAIRED);
}

static void test_intentos_agotados_bloquean(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint8_t good[PAIRING_SECRET_LEN], bad[PAIRING_SECRET_LEN];
    fill_secret(good, 0x30);
    fill_secret(bad, 0x99);
    assert(pairing_open_window(&p, 1000, good) == PAIR_OK);

    pairing_hello_t cl = client_hello(ALL_CAPS);
    uint64_t id = 0;
    assert(pairing_complete(&p, 1100, bad, &cl, &id) == PAIR_ERR_BAD_SECRET);
    assert(pairing_complete(&p, 1200, bad, &cl, &id) == PAIR_ERR_BAD_SECRET);
    /* Tercer fallo: se bloquea. La ventana temporal sola no impide probar secretos en
     * serie; el límite de intentos sí. */
    assert(pairing_complete(&p, 1300, bad, &cl, &id) == PAIR_ERR_TOO_MANY);
    assert(pairing_state(&p) == PAIR_STATE_LOCKED);
    /* Y ya no sirve ni el secreto correcto: hace falta otra acción física. */
    assert(pairing_complete(&p, 1400, good, &cl, &id) == PAIR_ERR_TOO_MANY);
}

static void test_accion_fisica_recupera_el_control(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint8_t good[PAIRING_SECRET_LEN], bad[PAIRING_SECRET_LEN];
    fill_secret(good, 0x40);
    fill_secret(bad, 0x77);
    assert(pairing_open_window(&p, 1000, bad) == PAIR_OK);
    pairing_hello_t cl = client_hello(ALL_CAPS);
    uint64_t id = 0;
    for (int i = 0; i < PAIRING_MAX_ATTEMPTS; ++i) {
        pairing_complete(&p, 1100 + (uint64_t)i * 100, good, &cl, &id);
    }
    assert(pairing_state(&p) == PAIR_STATE_LOCKED);

    /* Reabrir la ventana desde el dispositivo desbloquea: es la única prueba de
     * posesión que un HUD sin teclado puede dar, y es lo que permite recuperar el
     * control tras perder el teléfono. */
    assert(pairing_open_window(&p, 5000, good) == PAIR_OK);
    assert(pairing_state(&p) == PAIR_STATE_WINDOW);
    assert(pairing_complete(&p, 5100, good, &cl, &id) == PAIR_OK);
}

/* ---------- posesión única ---------- */

static void test_vincular_otro_revoca_el_anterior(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t first = bond(&p, 1000, 0x50);

    /* Sesión abierta del primero. */
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    assert(pairing_open_session(&p, first, PAIR_CAP_MAP_INSTALL, &sid, token) == PAIR_OK);
    assert(pairing_check_session(&p, sid, token) == PAIR_OK);

    /* Se vincula otro teléfono. */
    uint64_t second = bond(&p, 10000, 0x60);
    assert(second != first);

    /* El primero ya no vale, y su sesión tampoco. Dos dueños simultáneos no tienen
     * forma de resolverse. */
    uint64_t sid2 = 0;
    assert(pairing_open_session(&p, first, PAIR_CAP_MAP_INSTALL, &sid2, token)
           == PAIR_ERR_WRONG_CLIENT);
    assert(pairing_check_session(&p, sid, token) == PAIR_ERR_STATE);
}

/* ---------- la sesión bulk está atada a la vinculación ---------- */

static void test_sesion_exige_vinculacion(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    /* Sin vinculación no hay sesión: no existe un canal que acepte bytes por su
     * cuenta. */
    assert(pairing_open_session(&p, 1, PAIR_CAP_MAP_INSTALL, &sid, token)
           == PAIR_ERR_NOT_PAIRED);
}

static void test_token_ajeno_se_rechaza(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t id = bond(&p, 1000, 0x70);
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    assert(pairing_open_session(&p, id, PAIR_CAP_OTA, &sid, token) == PAIR_OK);

    uint8_t forged[PAIRING_TOKEN_LEN];
    memset(forged, 0xEE, sizeof(forged));
    assert(pairing_check_session(&p, sid, forged) == PAIR_ERR_WRONG_CLIENT);
    /* Y un id de sesión inventado tampoco. */
    assert(pairing_check_session(&p, sid + 7, token) == PAIR_ERR_STATE);
}

static void test_capacidad_no_declarada_no_da_sesion(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t id = bond(&p, 1000, 0x80);
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    /* BULK_WIFI no está implementada, así que el dispositivo no la declara y no se
     * concede una sesión para algo que no existe. */
    assert(pairing_open_session(&p, id, PAIR_CAP_BULK_WIFI, &sid, token)
           == PAIR_ERR_NO_CAP);
}

static void test_revocacion_invalida_la_sesion_en_curso(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t id = bond(&p, 1000, 0x90);
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    assert(pairing_open_session(&p, id, PAIR_CAP_MAP_INSTALL, &sid, token) == PAIR_OK);
    assert(pairing_check_session(&p, sid, token) == PAIR_OK);

    assert(pairing_revoke(&p) == PAIR_OK);
    /* Inmediata: el token deja de valer ahora, aunque la transferencia esté a mitad
     * de camino. Esperar un vencimiento dejaría al teléfono perdido operando. */
    assert(pairing_check_session(&p, sid, token) == PAIR_ERR_REVOKED);
    assert(pairing_state(&p) == PAIR_STATE_UNPAIRED);
    assert(pairing_revoke(&p) == PAIR_ERR_NOT_PAIRED);
}

static void test_cerrar_sesion_no_revoca(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t id = bond(&p, 1000, 0xA0);
    uint64_t sid = 0;
    uint8_t token[PAIRING_TOKEN_LEN];
    assert(pairing_open_session(&p, id, PAIR_CAP_OTA, &sid, token) == PAIR_OK);
    assert(pairing_close_session(&p, sid) == PAIR_OK);
    assert(pairing_state(&p) == PAIR_STATE_PAIRED);
    /* Y se puede abrir otra sesión sin volver a vincular. */
    uint64_t sid2 = 0;
    assert(pairing_open_session(&p, id, PAIR_CAP_OTA, &sid2, token) == PAIR_OK);
    assert(sid2 != sid);
}

static void test_tokens_distintos_por_sesion(void)
{
    pairing_t p;
    pairing_init(&p, NULL);
    uint64_t id = bond(&p, 1000, 0xB0);
    uint64_t s1 = 0, s2 = 0;
    uint8_t t1[PAIRING_TOKEN_LEN], t2[PAIRING_TOKEN_LEN];
    assert(pairing_open_session(&p, id, PAIR_CAP_OTA, &s1, t1) == PAIR_OK);
    assert(pairing_close_session(&p, s1) == PAIR_OK);
    assert(pairing_open_session(&p, id, PAIR_CAP_OTA, &s2, t2) == PAIR_OK);
    /* Reusar el token de una sesión cerrada no puede funcionar. */
    assert(memcmp(t1, t2, PAIRING_TOKEN_LEN) != 0);
    assert(pairing_check_session(&p, s2, t1) == PAIR_ERR_WRONG_CLIENT);
}

static void test_punteros_nulos(void)
{
    pairing_init(NULL, NULL);
    assert(pairing_open_window(NULL, 0, NULL) == PAIR_ERR_ARG);
    assert(pairing_complete(NULL, 0, NULL, NULL, NULL) == PAIR_ERR_ARG);
    assert(pairing_revoke(NULL) == PAIR_ERR_ARG);
    assert(pairing_open_session(NULL, 0, PAIR_CAP_OTA, NULL, NULL) == PAIR_ERR_ARG);
    assert(pairing_check_session(NULL, 0, NULL) == PAIR_ERR_ARG);
    assert(pairing_close_session(NULL, 0) == PAIR_ERR_ARG);
    assert(pairing_close_window(NULL) == PAIR_ERR_ARG);
    assert(pairing_state(NULL) == PAIR_STATE_UNPAIRED);
    assert(pairing_has_capability(NULL, PAIR_CAP_OTA) == false);

    pairing_negotiation_t n = pairing_negotiate(NULL, NULL);
    assert(n.ok == false);

    assert(strcmp(pairing_state_name(PAIR_STATE_LOCKED), "bloqueado") == 0);
    assert(strcmp(pairing_err_name(PAIR_ERR_REVOKED), "revocado") == 0);
}

int main(void)
{
    test_negociacion_elige_la_mas_alta_comun();
    test_negociacion_sin_version_comun();
    test_cliente_mas_nuevo_degrada();
    test_vinculacion_completa();
    test_fuera_de_la_ventana_se_rechaza();
    test_intentos_agotados_bloquean();
    test_accion_fisica_recupera_el_control();
    test_vincular_otro_revoca_el_anterior();
    test_sesion_exige_vinculacion();
    test_token_ajeno_se_rechaza();
    test_capacidad_no_declarada_no_da_sesion();
    test_revocacion_invalida_la_sesion_en_curso();
    test_cerrar_sesion_no_revoca();
    test_tokens_distintos_por_sesion();
    test_punteros_nulos();
    printf("pairing tests passed\n");
    return 0;
}
