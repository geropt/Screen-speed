/* Framer de sentencias NMEA seguro frente a datos binarios.
 *
 * Problema que resuelve. El HUD trataba el enlace del Ruptela como texto ASCII:
 * pedía al driver de UART que detectara el patrón '\n' y leía hasta ahí, después
 * recorría el buffer con `while (*d)` y usaba '\r' como fin de sentencia. En un
 * canal transparente que además transporta records binarios eso se rompe de tres
 * formas, todas reales:
 *
 *   - un 0x24 dentro de un record binario se interpreta como '$' y reinicia el
 *     parseo a mitad de camino;
 *   - un 0x0A dentro del payload dispara una detección de patrón espuria y parte
 *     el frame en dos lecturas;
 *   - un 0x00 corta el recorrido y descarta el resto del bloque leído.
 *
 * Diseño de este framer:
 *
 *   - Entra un flujo de bytes arbitrario. No hay terminador NUL, no hay
 *     dependencia de '\n' ni de '\r' como frontera de transporte.
 *   - Una sentencia **termina por estructura**: '*' seguido de exactamente dos
 *     dígitos hexadecimales. Ahí se valida el checksum XOR y, sólo si coincide, se
 *     entrega. El CR/LF que venga después es ruido irrelevante.
 *   - Una sentencia **empieza** en '$'. Un '$' a mitad de otra sentencia la
 *     descarta y arranca de nuevo: es lo correcto tanto ante un corte de enlace
 *     como ante un 0x24 binario.
 *   - Cualquier byte de control (< 0x20) o 0x00 dentro de una sentencia sin
 *     cerrar la aborta y la cuenta. Nada revienta y nada se publica.
 *   - Desbordar el máximo de sentencia la descarta y la cuenta; el framer vuelve
 *     a esperar un '$'.
 *
 * Todo el estado vive en `nmea_framer_t`, con reset explícito. Sin dependencias
 * de ESP-IDF: se prueba en el host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NMEA 0183 limita la sentencia a 82 caracteres incluyendo '$' y CRLF. Se toman
 * 100 para tolerar equipos que se pasan un poco sin descartar datos útiles. */
#define NMEA_FRAMER_MAX_SENTENCE 100

typedef struct {
    uint32_t sentences_ok;      /**< entregadas con checksum válido           */
    uint32_t crc_errors;        /**< checksum declarado que no coincide        */
    uint32_t aborted_control;   /**< byte de control dentro de una sentencia   */
    uint32_t aborted_restart;   /**< '$' antes de cerrar la anterior           */
    uint32_t overflow;          /**< sentencia más larga que el máximo         */
    uint32_t bad_checksum_char; /**< carácter no hexadecimal donde iba el CRC  */
    uint32_t noise_bytes;       /**< bytes fuera de toda sentencia             */
} nmea_framer_stats_t;

/**
 * @brief Se invoca por cada sentencia completa y con checksum válido.
 *
 * @param ctx      contexto del usuario
 * @param sentence texto de la sentencia, desde '$' hasta el segundo dígito del
 *                 checksum, terminado en NUL por comodidad del consumidor
 * @param len      longitud sin contar el NUL
 */
typedef void (*nmea_framer_cb_t)(void *ctx, const char *sentence, size_t len);

typedef struct {
    char     buf[NMEA_FRAMER_MAX_SENTENCE];
    size_t   len;
    bool     in_sentence;
    bool     asterisk;          /**< ya se vio el '*'                        */
    uint8_t  crc;               /**< XOR acumulado entre '$' y '*'           */
    uint8_t  declared;          /**< checksum declarado, mientras se arma    */
    uint8_t  declared_digits;   /**< 0, 1 o 2                                */
    nmea_framer_cb_t cb;
    void    *ctx;
    nmea_framer_stats_t stats;
} nmea_framer_t;

/** Inicializa el framer y deja los contadores en cero. */
void nmea_framer_init(nmea_framer_t *f, nmea_framer_cb_t cb, void *ctx);

/**
 * @brief Descarta la sentencia a medio armar y limpia los contadores.
 *
 * Usar cuando el enlace cambió (por ejemplo al cambiar de baud) y lo acumulado
 * ya no tiene sentido.
 */
void nmea_framer_reset(nmea_framer_t *f);

/**
 * @brief Descarta sólo la sentencia a medio armar, conservando contadores.
 *
 * Para overflow del ring de UART: lo que sigue no es continuación de nada.
 */
void nmea_framer_discard_partial(nmea_framer_t *f);

/** Alimenta bytes crudos. Puede llamarse con cortes en cualquier posición. */
void nmea_framer_feed(nmea_framer_t *f, const uint8_t *data, size_t len);

/** Copia los contadores. */
void nmea_framer_get_stats(const nmea_framer_t *f, nmea_framer_stats_t *out);

/**
 * @brief Checksum XOR de un cuerpo de sentencia, para pruebas y herramientas.
 *
 * @param body bytes entre '$' y '*', ambos excluidos
 */
uint8_t nmea_framer_checksum(const char *body, size_t len);

#ifdef __cplusplus
}
#endif
