/* Dueño único del bus I2C de la placa.
 *
 * El touch, el PMIC y el RTC comparten SDA/SCL. Hoy el firmware no inicializa
 * I2C en absoluto (el bloque de touch está compilado fuera con USE_TOUCH 0), así
 * que no hay conflicto todavía; este componente existe para que cuando se
 * incorpore el primer periférico haya un solo lugar que instale el driver y un
 * solo criterio ante ausencia o error.
 *
 * Reglas:
 *  - `board_i2c_acquire()` es idempotente: el primer llamador instala el bus, el
 *    resto recibe ESP_OK. Ningún periférico instala el driver por su cuenta.
 *  - La ausencia de un periférico NO es un fallo del sistema: `board_i2c_probe()`
 *    devuelve ESP_ERR_NOT_FOUND y el llamador debe seguir funcionando sin él.
 *  - Se usa la API legacy (`driver/i2c.h`) porque es la que espera SensorLib,
 *    que es el driver del touch ya presente en el árbol. Migrar a
 *    `driver/i2c_master.h` es un cambio propio, con su prueba en la placa.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Puerto usado por la placa para los periféricos compartidos. */
#define BOARD_I2C_PORT  ((i2c_port_t)I2C_NUM_1)

/**
 * @brief Instala el bus I2C si todavía no está instalado.
 *
 * @return ESP_OK si el bus quedó disponible (recién instalado o ya lo estaba),
 *         o el error de `i2c_param_config`/`i2c_driver_install`.
 */
esp_err_t board_i2c_acquire(void);

/** @return true si el bus ya está instalado por este componente. */
bool board_i2c_is_ready(void);

/**
 * @brief Comprueba si un dispositivo responde en una dirección.
 *
 * Pensado para incorporar periféricos de a uno y distinguir «no está» de «falló
 * el bus».
 *
 * @param addr_7bit dirección de 7 bits
 * @param timeout_ms espera máxima
 * @return ESP_OK si responde, ESP_ERR_NOT_FOUND si no hay ACK, otro error si el
 *         bus falló o no está adquirido.
 */
esp_err_t board_i2c_probe(uint8_t addr_7bit, uint32_t timeout_ms);

/**
 * @brief Lee registros de un dispositivo del bus.
 *
 * Firma compatible con `iic_fptr_t` de SensorLib, para poder usar la variante de
 * `begin()` basada en callbacks: así el driver del periférico nunca instala ni
 * configura el bus. Es la única forma de que el dueño único sea real —
 * `TouchDrvCST92xx::begin(i2c_port_t, ...)` llama a `i2c_driver_install()` por su
 * cuenta (SensorLib, SensorCommon.tpp).
 *
 * @return 0 si salió bien, -1 si falló
 */
int board_i2c_read_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);

/**
 * @brief Escribe registros de un dispositivo del bus.
 *
 * Contraparte de `board_i2c_read_regs`, misma firma `iic_fptr_t`.
 *
 * @return 0 si salió bien, -1 si falló
 */
int board_i2c_write_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len);

/**
 * @brief Contadores acumulados del bus, para el log periódico de recursos.
 *
 * @param out_probes    sondeos realizados
 * @param out_absent    sondeos sin ACK (periférico ausente)
 * @param out_bus_errs  errores de bus (timeout, arbitraje, driver)
 */
void board_i2c_stats(uint32_t *out_probes, uint32_t *out_absent,
                     uint32_t *out_bus_errs);

#ifdef __cplusplus
}
#endif
