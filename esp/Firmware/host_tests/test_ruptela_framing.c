/* Pruebas del framing compartido: nmea_framer + imei_scanner.
 *
 * Cubre la fila «Framing» de la matriz mínima de fallas del plan: binario con
 * NUL/newline/'$', mensajes concatenados, corte en cada offset, CRC malo,
 * colisiones, ruido largo y overflow.
 *
 * La propiedad central que se prueba es que el fin de sentencia lo define la
 * estructura ('*' + dos hex), no un newline: las sentencias se reconocen sin que
 * el flujo traiga jamás un '\n' ni un terminador NUL.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nmea_framer.h"
#include "imei_scanner.h"

/* ---------- utilidades ---------- */

#define MAX_CAPTURED 16

typedef struct {
    char   sentences[MAX_CAPTURED][NMEA_FRAMER_MAX_SENTENCE];
    size_t lens[MAX_CAPTURED];
    size_t count;
} capture_t;

static void on_sentence(void *ctx, const char *sentence, size_t len)
{
    capture_t *c = (capture_t *)ctx;
    assert(c->count < MAX_CAPTURED);
    assert(len < NMEA_FRAMER_MAX_SENTENCE);
    memcpy(c->sentences[c->count], sentence, len + 1);
    c->lens[c->count] = len;
    c->count++;
}

/* Arma "$<body>*<CS>" con el checksum correcto. Devuelve la longitud. */
static size_t build_sentence(char *out, size_t out_sz, const char *body)
{
    uint8_t crc = nmea_framer_checksum(body, strlen(body));
    int n = snprintf(out, out_sz, "$%s*%02X", body, crc);
    assert(n > 0 && (size_t)n < out_sz);
    return (size_t)n;
}

static const char RMC_BODY[] =
    "GPRMC,123519,A,3451.1000,S,05829.2000,W,022.4,084.4,230326,003.1,W";

/* ---------- nmea_framer ---------- */

static void test_sentencia_valida_sin_newline(void)
{
    /* Ni '\n' ni '\r' ni NUL en el flujo: la sentencia se cierra por estructura. */
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, n);

    assert(cap.count == 1);
    assert(strcmp(cap.sentences[0], s) == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.sentences_ok == 1);
    assert(st.crc_errors == 0);
}

static void test_crlf_posterior_es_ruido(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);
    uint8_t stream[160];
    memcpy(stream, s, n);
    stream[n] = '\r';
    stream[n + 1] = '\n';

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, stream, n + 2);

    /* Se entrega una sola vez, y el CR/LF no aborta nada porque la sentencia ya
     * estaba cerrada: cuentan como ruido fuera de sentencia. */
    assert(cap.count == 1);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.sentences_ok == 1);
    assert(st.aborted_control == 0);
    assert(st.noise_bytes == 2);
}

static void test_mensajes_concatenados(void)
{
    char a[128], b[128];
    size_t na = build_sentence(a, sizeof(a), RMC_BODY);
    size_t nb = build_sentence(b, sizeof(b), "GPGGA,123519,3451.1000,S,05829.2000,W,1,08,0.9,545.4,M");

    uint8_t stream[300];
    size_t n = 0;
    memcpy(stream + n, a, na); n += na;
    memcpy(stream + n, b, nb); n += nb;   /* pegadas, sin separador alguno */

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, stream, n);

    assert(cap.count == 2);
    assert(strcmp(cap.sentences[0], a) == 0);
    assert(strcmp(cap.sentences[1], b) == 0);
}

static void test_checksum_malo_no_publica(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);
    /* Corromper el último dígito del checksum. */
    s[n - 1] = (s[n - 1] == '0') ? '1' : '0';

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, n);

    assert(cap.count == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.crc_errors == 1);
    assert(st.sentences_ok == 0);
}

static void test_cuerpo_corrompido_falla_checksum(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);
    s[10] = (s[10] == '1') ? '2' : '1';   /* dentro del cuerpo */

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, n);

    assert(cap.count == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.crc_errors == 1);
}

static void test_corte_en_cada_offset(void)
{
    /* La misma sentencia partida en dos entregas, con el corte en cada posición
     * posible. En los 0..n cortes el resultado debe ser idéntico. */
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    for (size_t cut = 0; cut <= n; ++cut) {
        capture_t cap = {0};
        nmea_framer_t f;
        nmea_framer_init(&f, on_sentence, &cap);
        nmea_framer_feed(&f, (const uint8_t *)s, cut);
        nmea_framer_feed(&f, (const uint8_t *)s + cut, n - cut);
        assert(cap.count == 1);
        assert(strcmp(cap.sentences[0], s) == 0);
    }
}

static void test_corte_byte_a_byte(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    for (size_t i = 0; i < n; ++i) {
        nmea_framer_feed(&f, (const uint8_t *)s + i, 1);
    }
    assert(cap.count == 1);
    assert(strcmp(cap.sentences[0], s) == 0);
}

static void test_binario_con_nul_newline_y_dolar(void)
{
    /* Este es el caso que rompía el transporte anterior: un bloque binario con
     * 0x00, 0x0A, 0x0D y 0x24, seguido de una sentencia buena. Antes el NUL
     * cortaba el recorrido, el 0x0A partía la lectura y el 0x24 reiniciaba el
     * parseo. Acá la sentencia posterior tiene que llegar intacta. */
    const uint8_t binario[] = {
        0x20, 0x19, 0x00, 0x00, 0x0A, 0x0D, 0x24, 0x00, 0xFF, 0x80,
        0x24, 0x0A, 0x00, 0x7F, 0x01, 0x02, 0x03, 0x24, 0x00, 0x0A,
    };
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    uint8_t stream[256];
    size_t len = 0;
    memcpy(stream + len, binario, sizeof(binario)); len += sizeof(binario);
    memcpy(stream + len, s, n); len += n;

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, stream, len);

    assert(cap.count == 1);
    assert(strcmp(cap.sentences[0], s) == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.sentences_ok == 1);
    /* Los '$' binarios abrieron sentencias que se abortaron por byte de control:
     * quedan contadas, no publicadas. */
    assert(st.aborted_control >= 1);
    assert(st.crc_errors == 0);
}

static void test_dolar_a_mitad_reinicia(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    uint8_t stream[256];
    size_t len = 0;
    /* Media sentencia, cortada sin cerrar, y después la sentencia completa. */
    memcpy(stream + len, s, 20); len += 20;
    memcpy(stream + len, s, n); len += n;

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, stream, len);

    assert(cap.count == 1);
    assert(strcmp(cap.sentences[0], s) == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.aborted_restart == 1);
}

static void test_ruido_largo_no_desborda(void)
{
    /* Ruido ASCII largo sin '$': no debe acumular nada ni perder la sentencia
     * que viene después. */
    uint8_t noise[4096];
    for (size_t i = 0; i < sizeof(noise); ++i) {
        noise[i] = (uint8_t)('A' + (i % 26));
    }
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, noise, sizeof(noise));
    nmea_framer_feed(&f, (const uint8_t *)s, n);

    assert(cap.count == 1);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.noise_bytes == sizeof(noise));
    assert(st.overflow == 0);
}

static void test_overflow_de_sentencia(void)
{
    /* '$' seguido de un cuerpo larguísimo sin '*': tiene que desbordar de forma
     * contada y no impedir que la siguiente sentencia se reconozca. */
    uint8_t big[NMEA_FRAMER_MAX_SENTENCE * 3];
    big[0] = '$';
    for (size_t i = 1; i < sizeof(big); ++i) {
        big[i] = 'X';
    }
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, big, sizeof(big));
    nmea_framer_feed(&f, (const uint8_t *)s, n);

    assert(cap.count == 1);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.overflow == 1);
}

static void test_checksum_no_hexadecimal(void)
{
    const char *bad = "$GPRMC,123519,A*ZZ";
    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)bad, strlen(bad));

    assert(cap.count == 0);
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.bad_checksum_char == 1);
}

static void test_checksum_minusculas(void)
{
    /* Algunos equipos emiten el checksum en minúsculas; es válido. */
    char body[] = "GPRMC,1,A";
    uint8_t crc = nmea_framer_checksum(body, strlen(body));
    char s[64];
    snprintf(s, sizeof(s), "$%s*%02x", body, crc);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, strlen(s));
    assert(cap.count == 1);
}

static void test_discard_partial(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);

    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, 20);
    /* Overflow del ring de UART: lo que venga no es continuación de esto. */
    nmea_framer_discard_partial(&f);
    nmea_framer_feed(&f, (const uint8_t *)s + 20, n - 20);
    /* La cola sin su encabezado no puede publicarse. */
    assert(cap.count == 0);

    nmea_framer_feed(&f, (const uint8_t *)s, n);
    assert(cap.count == 1);
}

static void test_reset_limpia_contadores(void)
{
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);
    capture_t cap = {0};
    nmea_framer_t f;
    nmea_framer_init(&f, on_sentence, &cap);
    nmea_framer_feed(&f, (const uint8_t *)s, n);
    nmea_framer_reset(&f);

    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.sentences_ok == 0);
    /* El callback sobrevive al reset. */
    nmea_framer_feed(&f, (const uint8_t *)s, n);
    assert(cap.count == 2);
}

static void test_punteros_nulos(void)
{
    nmea_framer_init(NULL, NULL, NULL);
    nmea_framer_reset(NULL);
    nmea_framer_discard_partial(NULL);
    nmea_framer_feed(NULL, NULL, 0);
    nmea_framer_get_stats(NULL, NULL);

    nmea_framer_t f;
    nmea_framer_init(&f, NULL, NULL);
    nmea_framer_feed(&f, NULL, 10);      /* datos nulos con longitud: no debe leer */
    char s[128];
    size_t n = build_sentence(s, sizeof(s), RMC_BODY);
    nmea_framer_feed(&f, (const uint8_t *)s, n);  /* sin callback registrado */
    nmea_framer_stats_t st;
    nmea_framer_get_stats(&f, &st);
    assert(st.sentences_ok == 1);
}

/* ---------- imei_scanner ---------- */

typedef struct {
    char   imeis[8][IMEI_SCANNER_MAX_DIGITS + 1];
    size_t count;
} imei_capture_t;

static void on_imei(void *ctx, const char *imei, size_t len)
{
    imei_capture_t *c = (imei_capture_t *)ctx;
    assert(c->count < 8);
    assert(len <= IMEI_SCANNER_MAX_DIGITS);
    strcpy(c->imeis[c->count++], imei);
}

static void test_imei_simple(void)
{
    const char *stream = "###IMEI356938035643809$GPRMC";
    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, (const uint8_t *)stream, strlen(stream));

    assert(cap.count == 1);
    assert(strcmp(cap.imeis[0], "356938035643809") == 0);
}

static void test_imei_corte_en_cada_offset(void)
{
    const char *stream = "ruido###IMEI356938035643809 despues";
    size_t n = strlen(stream);

    for (size_t cut = 0; cut <= n; ++cut) {
        imei_capture_t cap = {0};
        imei_scanner_t s;
        imei_scanner_init(&s, on_imei, &cap);
        imei_scanner_feed(&s, (const uint8_t *)stream, cut);
        imei_scanner_feed(&s, (const uint8_t *)stream + cut, n - cut);
        assert(cap.count == 1);
        assert(strcmp(cap.imeis[0], "356938035643809") == 0);
    }
}

static void test_imei_16_digitos_cierra_solo(void)
{
    /* Con 16 dígitos se entrega sin esperar un byte no numérico. */
    const char *stream = "###IMEI3569380356438091";
    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, (const uint8_t *)stream, strlen(stream));
    assert(cap.count == 1);
    assert(strlen(cap.imeis[0]) == 16);
}

static void test_imei_demasiado_corto(void)
{
    const char *stream = "###IMEI123 ";
    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, (const uint8_t *)stream, strlen(stream));

    assert(cap.count == 0);
    imei_scanner_stats_t st;
    imei_scanner_get_stats(&s, &st);
    assert(st.too_short == 1);
}

static void test_imei_prefijo_con_numerales_extra(void)
{
    /* "#####IMEI..." tiene que reconocerse: el reintento del primer carácter del
     * prefijo es lo que lo permite. */
    const char *stream = "#####IMEI356938035643809 ";
    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, (const uint8_t *)stream, strlen(stream));
    assert(cap.count == 1);
    assert(strcmp(cap.imeis[0], "356938035643809") == 0);
}

static void test_imei_dos_marcadores(void)
{
    const char *stream = "###IMEI356938035643809 x ###IMEI356938035643801 y";
    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, (const uint8_t *)stream, strlen(stream));
    assert(cap.count == 2);
    assert(strcmp(cap.imeis[1], "356938035643801") == 0);
}

static void test_imei_entre_binario(void)
{
    uint8_t stream[128];
    size_t n = 0;
    const uint8_t bin[] = {0x00, 0x0A, 0x24, 0xFF, 0x1B};
    memcpy(stream + n, bin, sizeof(bin)); n += sizeof(bin);
    const char *marker = "###IMEI356938035643809";
    memcpy(stream + n, marker, strlen(marker)); n += strlen(marker);
    stream[n++] = 0x00;   /* un NUL cierra la corrida de dígitos */

    imei_capture_t cap = {0};
    imei_scanner_t s;
    imei_scanner_init(&s, on_imei, &cap);
    imei_scanner_feed(&s, stream, n);
    assert(cap.count == 1);
}

static void test_imei_punteros_nulos(void)
{
    imei_scanner_init(NULL, NULL, NULL);
    imei_scanner_reset(NULL);
    imei_scanner_feed(NULL, NULL, 0);
    imei_scanner_get_stats(NULL, NULL);
}

int main(void)
{
    test_sentencia_valida_sin_newline();
    test_crlf_posterior_es_ruido();
    test_mensajes_concatenados();
    test_checksum_malo_no_publica();
    test_cuerpo_corrompido_falla_checksum();
    test_corte_en_cada_offset();
    test_corte_byte_a_byte();
    test_binario_con_nul_newline_y_dolar();
    test_dolar_a_mitad_reinicia();
    test_ruido_largo_no_desborda();
    test_overflow_de_sentencia();
    test_checksum_no_hexadecimal();
    test_checksum_minusculas();
    test_discard_partial();
    test_reset_limpia_contadores();
    test_punteros_nulos();

    test_imei_simple();
    test_imei_corte_en_cada_offset();
    test_imei_16_digitos_cierra_solo();
    test_imei_demasiado_corto();
    test_imei_prefijo_con_numerales_extra();
    test_imei_dos_marcadores();
    test_imei_entre_binario();
    test_imei_punteros_nulos();

    printf("ruptela_framing tests passed\n");
    return 0;
}
