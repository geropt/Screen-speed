#include "nmea_framer.h"

#include <string.h>

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void abandon(nmea_framer_t *f)
{
    f->in_sentence = false;
    f->asterisk = false;
    f->len = 0;
    f->crc = 0;
    f->declared = 0;
    f->declared_digits = 0;
}

static void start(nmea_framer_t *f)
{
    f->in_sentence = true;
    f->asterisk = false;
    f->len = 0;
    f->crc = 0;
    f->declared = 0;
    f->declared_digits = 0;
    f->buf[f->len++] = '$';
}

/* Agrega un byte al texto de la sentencia. Devuelve false si no cabe. */
static bool push(nmea_framer_t *f, uint8_t c)
{
    /* Se reserva un byte para el NUL final. */
    if (f->len + 1 >= NMEA_FRAMER_MAX_SENTENCE) {
        return false;
    }
    f->buf[f->len++] = (char)c;
    return true;
}

void nmea_framer_init(nmea_framer_t *f, nmea_framer_cb_t cb, void *ctx)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->ctx = ctx;
}

void nmea_framer_reset(nmea_framer_t *f)
{
    if (!f) {
        return;
    }
    nmea_framer_cb_t cb = f->cb;
    void *ctx = f->ctx;
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->ctx = ctx;
}

void nmea_framer_discard_partial(nmea_framer_t *f)
{
    if (!f) {
        return;
    }
    abandon(f);
}

uint8_t nmea_framer_checksum(const char *body, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint8_t)body[i];
    }
    return crc;
}

void nmea_framer_feed(nmea_framer_t *f, const uint8_t *data, size_t len)
{
    if (!f || (!data && len)) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];

        /* '$' manda siempre: abre sentencia, aunque haya una a medio armar. Un
         * 0x24 dentro de un record binario entra por acá y como mucho produce una
         * sentencia que después se aborta o falla checksum; nunca corrompe otra ya
         * en curso sin dejar rastro. */
        if (c == '$') {
            if (f->in_sentence) {
                f->stats.aborted_restart++;
            }
            start(f);
            continue;
        }

        if (!f->in_sentence) {
            f->stats.noise_bytes++;
            continue;
        }

        /* Byte de control o NUL dentro de una sentencia sin cerrar: no puede ser
         * parte de ella. Se aborta. Notar que un CR/LF *después* del checksum no
         * llega acá, porque la sentencia ya se cerró y entregó. */
        if (c < 0x20 || c == 0x7F) {
            f->stats.aborted_control++;
            abandon(f);
            continue;
        }

        if (!f->asterisk) {
            if (c == '*') {
                f->asterisk = true;
                if (!push(f, c)) {
                    f->stats.overflow++;
                    abandon(f);
                }
                continue;
            }
            f->crc ^= c;
            if (!push(f, c)) {
                f->stats.overflow++;
                abandon(f);
            }
            continue;
        }

        /* Después del '*' van exactamente dos dígitos hexadecimales. */
        const int v = hex_value(c);
        if (v < 0) {
            f->stats.bad_checksum_char++;
            abandon(f);
            continue;
        }
        if (!push(f, c)) {
            f->stats.overflow++;
            abandon(f);
            continue;
        }
        f->declared = (uint8_t)((f->declared << 4) | (uint8_t)v);
        f->declared_digits++;

        if (f->declared_digits < 2) {
            continue;
        }

        /* Sentencia cerrada por estructura. Acá se decide si se publica. */
        if (f->declared == f->crc) {
            f->stats.sentences_ok++;
            f->buf[f->len] = '\0';
            if (f->cb) {
                f->cb(f->ctx, f->buf, f->len);
            }
        } else {
            f->stats.crc_errors++;
        }
        abandon(f);
    }
}

void nmea_framer_get_stats(const nmea_framer_t *f, nmea_framer_stats_t *out)
{
    if (!f || !out) {
        return;
    }
    *out = f->stats;
}
