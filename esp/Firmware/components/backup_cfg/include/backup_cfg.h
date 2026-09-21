/* Configuración de respaldo independiente del build.
 *
 * Host, puerto y redes no son secretos de flota en el binario: el valor de
 * Kconfig es sólo fábrica. El texto se parsea igual en el host y en la placa.
 * Quién lee el archivo (SD) y quién persiste (NVS) queda fuera de este módulo.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKUP_CFG_HOST_MAX 127
#define BACKUP_CFG_SSID_MAX 32
#define BACKUP_CFG_PASS_MAX 64
#define BACKUP_CFG_MAX_NETS 4

typedef enum {
    BACKUP_CFG_OK = 0,
    BACKUP_CFG_ERR_EMPTY,    /**< sin claves útiles                          */
    BACKUP_CFG_ERR_SYNTAX,   /**< línea sin '=', clave vacía o desconocida   */
    BACKUP_CFG_ERR_HOST,     /**< host vacío o con caracteres no DNS         */
    BACKUP_CFG_ERR_PORT,     /**< puerto fuera de 1..65535                   */
    BACKUP_CFG_ERR_NET,      /**< ssid/password demasiado largo o huérfano   */
} backup_cfg_err_t;

typedef struct {
    char ssid[BACKUP_CFG_SSID_MAX + 1];
    char pass[BACKUP_CFG_PASS_MAX + 1];
    bool have_pass;   /**< apareció la clave password para este ssid */
} backup_cfg_net_t;

typedef struct {
    char     host[BACKUP_CFG_HOST_MAX + 1];
    uint16_t port;
    bool     have_host;
    bool     have_port;
    backup_cfg_net_t nets[BACKUP_CFG_MAX_NETS];
    size_t   net_count;
} backup_cfg_t;

/**
 * @brief Parsea un texto `clave=valor` (una por línea).
 *
 * Líneas vacías y comentarios `#` se ignoran. Claves: `host`, `port`, `ssid`,
 * `password`. Cada `ssid` abre una red; el `password` siguiente le pertenece.
 * No interpreta comillas. Recorta espacios alrededor de clave y valor.
 *
 * @return BACKUP_CFG_OK si hay al menos host, puerto o una red.
 */
backup_cfg_err_t backup_cfg_parse(const char *text, backup_cfg_t *out);

const char *backup_cfg_err_name(backup_cfg_err_t err);

#ifdef __cplusplus
}
#endif
