/* Detector del marcador `###IMEI<dígitos>` que el Ruptela intercala en el canal
 * transparente.
 *
 * El marcador no viene delimitado por newline ni por NUL: aparece entre
 * sentencias NMEA y records binarios, y puede quedar partido entre dos lecturas
 * de UART. Este scanner mantiene el arrastre necesario para reconocerlo aunque el
 * corte caiga en cualquier posición, incluso en medio del prefijo o de los
 * dígitos.
 *
 * Reglas:
 *  - El prefijo es `###IMEI` exacto.
 *  - Después se aceptan entre 14 y 16 dígitos. Se entrega al llegar a 16 o
 *    cuando aparece un byte que no es dígito habiendo ya al menos 14.
 *  - Menos de 14 dígitos seguidos de otro byte es un marcador roto: se cuenta y
 *    se descarta.
 *
 * Sin dependencias de ESP-IDF: se prueba en el host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMEI_SCANNER_PREFIX     "###IMEI"
#define IMEI_SCANNER_PREFIX_LEN 7
#define IMEI_SCANNER_MIN_DIGITS 14
#define IMEI_SCANNER_MAX_DIGITS 16

typedef struct {
    uint32_t found;      /**< marcadores completos entregados      */
    uint32_t too_short;  /**< prefijo con menos de 14 dígitos       */
} imei_scanner_stats_t;

/**
 * @brief Se invoca con el IMEI en texto, terminado en NUL.
 *
 * @param len cantidad de dígitos
 */
typedef void (*imei_scanner_cb_t)(void *ctx, const char *imei, size_t len);

typedef struct {
    /* Ventana con los últimos bytes vistos, para reconocer el prefijo aunque el
     * corte de UART caiga en cualquier posición y aunque venga solapado
     * (`#####IMEI`). Una ventana deslizante es correcta por construcción; un
     * reintento de un solo carácter no lo es. */
    uint8_t window[IMEI_SCANNER_PREFIX_LEN];
    uint8_t window_len;
    /* Dígitos acumulados cuando el prefijo ya está completo. */
    bool    collecting;
    char    digits[IMEI_SCANNER_MAX_DIGITS + 1];
    uint8_t digit_count;
    imei_scanner_cb_t cb;
    void   *ctx;
    imei_scanner_stats_t stats;
} imei_scanner_t;

void imei_scanner_init(imei_scanner_t *s, imei_scanner_cb_t cb, void *ctx);

/** Descarta la coincidencia parcial y limpia los contadores. */
void imei_scanner_reset(imei_scanner_t *s);

/** Alimenta bytes crudos; los cortes pueden caer en cualquier posición. */
void imei_scanner_feed(imei_scanner_t *s, const uint8_t *data, size_t len);

void imei_scanner_get_stats(const imei_scanner_t *s, imei_scanner_stats_t *out);

#ifdef __cplusplus
}
#endif
