/* Dueño único de la radio y de la configuración de red.
 *
 * Problema que resuelve. En el piloto, el Wi-Fi se prende y se apaga desde la misma
 * lógica que arma paquetes y habla con el servidor: `telematics.c` mezcla credenciales
 * de build, event groups, sockets y la máquina de estados del respaldo, con mucho
 * estado de archivo y sin ninguna prueba. Cuando aparezcan un segundo consumidor —la
 * app de teléfono en P08, el OTA en P09— ese diseño no alcanza: dos módulos peleando
 * por la radio no pueden coordinarse si cada uno la enciende por su cuenta.
 *
 * Reglas:
 *
 *  - **Un solo dueño.** Nadie más llama a `esp_wifi_start()`, `esp_wifi_connect()` ni
 *    toca netif. Los consumidores piden y esperan.
 *  - **Solicitudes por consumidor.** Cada consumidor tiene su handle y pide conexión;
 *    la radio se levanta con el primer pedido y se baja cuando no queda ninguno.
 *    Así el respaldo no apaga la radio que está usando una transferencia de mapas.
 *  - **Backoff con techo.** Reintentar sin pausa contra un AP que no responde gasta
 *    energía y llena el log; el backoff crece y se corta en un máximo.
 *  - **Esperas cancelables.** Toda espera —asociación, DHCP, DNS, connect— se hace
 *    con timeout y se puede abortar. El plan lo pide explícitamente: una revocación
 *    de permiso tiene que poder interrumpir una espera en curso, y sin esto habría
 *    que aguantar hasta el timeout del stack.
 *  - **Nada de borrado global de NVS ni credenciales de build.** El piloto hace las
 *    dos cosas y el plan lo prohíbe para el producto: las redes se guardan y se
 *    borran individualmente.
 *
 * LO QUE ESTE COMPONENTE NO PUEDE PROBAR SIN HARDWARE: absolutamente todo su
 * comportamiento. Es una envoltura de la radio. Su valor acá es el contrato y la
 * propiedad exclusiva; la verificación necesita la placa y un AP.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONN_SSID_MAX      32
#define CONN_PASS_MAX      64
#define CONN_MAX_NETWORKS  4
#define CONN_MAX_CONSUMERS 4

/** Quién pide la radio. Fijos y pocos: cada uno es un dueño identificable. */
typedef enum {
    CONN_CONSUMER_BACKUP = 0,   /**< respaldo de telemetría        */
    CONN_CONSUMER_MAPS,         /**< instalación de mapas (P07/P08) */
    CONN_CONSUMER_OTA,          /**< actualización de firmware (P09)*/
    CONN_CONSUMER_DIAG,         /**< diagnóstico manual             */
    CONN_CONSUMER_COUNT
} conn_consumer_t;

typedef enum {
    CONN_STATE_OFF = 0,      /**< radio apagada, nadie pidió        */
    CONN_STATE_CONNECTING,   /**< asociando o esperando IP          */
    CONN_STATE_CONNECTED,    /**< con IP usable                     */
    CONN_STATE_BACKOFF,      /**< falló, esperando para reintentar  */
    CONN_STATE_NO_CONFIG     /**< nadie configuró una red           */
} conn_state_t;

typedef struct {
    uint32_t backoff_initial_ms;
    uint32_t backoff_max_ms;
    /** Espera máxima por asociación más IP. */
    uint32_t connect_timeout_ms;
} conn_config_t;

#define CONN_CONFIG_DEFAULT() {   \
    .backoff_initial_ms = 2000,   \
    .backoff_max_ms = 60000,      \
    .connect_timeout_ms = 15000,  \
}

typedef struct {
    conn_state_t state;
    uint32_t     attempts;
    uint32_t     failures;
    uint32_t     disconnects;
    uint32_t     current_backoff_ms;
    uint8_t      active_requests;
    bool         have_ip;
} conn_status_t;

/**
 * @brief Inicializa el componente sin encender la radio.
 *
 * No conecta: la radio se levanta con el primer `conn_request()`. Un HUD que nunca
 * necesita red no debería gastar energía en Wi-Fi.
 */
esp_err_t conn_init(const conn_config_t *cfg);

/**
 * @brief Guarda una red. Persistente entre arranques.
 *
 * Reemplaza la entrada si el SSID ya existía. Nunca borra las demás: el plan prohíbe
 * el borrado global de NVS que hace el piloto.
 */
esp_err_t conn_add_network(const char *ssid, const char *password);

/** Borra una red por SSID. Sólo esa. */
esp_err_t conn_forget_network(const char *ssid);

/** @return cantidad de redes guardadas. */
size_t conn_network_count(void);

/**
 * @brief Un consumidor pide conexión.
 *
 * Idempotente por consumidor. La radio se levanta con el primer pedido activo.
 */
esp_err_t conn_request(conn_consumer_t who);

/**
 * @brief Un consumidor libera su pedido.
 *
 * Cuando no queda ninguno, la radio se apaga. Así el respaldo no puede apagar la
 * radio que está usando una transferencia de mapas.
 */
esp_err_t conn_release(conn_consumer_t who);

/**
 * @brief Espera hasta tener IP, o hasta que se cancele.
 *
 * @param timeout_ms espera máxima
 * @return ESP_OK con IP; ESP_ERR_TIMEOUT si venció; ESP_ERR_INVALID_STATE si se
 *         canceló con `conn_cancel_waits()`.
 */
esp_err_t conn_wait_connected(uint32_t timeout_ms);

/**
 * @brief Cancela toda espera en curso y las siguientes hasta rearmar.
 *
 * Es el mecanismo que hace cancelables las esperas: una revocación de permiso llama
 * a esto y quien estaba esperando vuelve de inmediato con ESP_ERR_INVALID_STATE, en
 * lugar de quedarse hasta el timeout del stack.
 */
void conn_cancel_waits(void);

/** Rearma las esperas después de una cancelación. */
void conn_resume_waits(void);

/** @return true si hay una cancelación vigente. */
bool conn_waits_cancelled(void);

void conn_get_status(conn_status_t *out);

const char *conn_state_name(conn_state_t s);
const char *conn_consumer_name(conn_consumer_t c);

#ifdef __cplusplus
}
#endif
