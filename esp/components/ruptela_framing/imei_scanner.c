#include "imei_scanner.h"

#include <string.h>

static const char PREFIX[IMEI_SCANNER_PREFIX_LEN] = {'#', '#', '#', 'I', 'M', 'E', 'I'};

static void emit_if_complete(imei_scanner_t *s)
{
    if (s->digit_count >= IMEI_SCANNER_MIN_DIGITS) {
        s->stats.found++;
        s->digits[s->digit_count] = '\0';
        if (s->cb) {
            s->cb(s->ctx, s->digits, s->digit_count);
        }
    } else {
        s->stats.too_short++;
    }
    s->collecting = false;
    s->digit_count = 0;
}

void imei_scanner_init(imei_scanner_t *s, imei_scanner_cb_t cb, void *ctx)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
}

void imei_scanner_reset(imei_scanner_t *s)
{
    if (!s) {
        return;
    }
    imei_scanner_cb_t cb = s->cb;
    void *ctx = s->ctx;
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
}

void imei_scanner_feed(imei_scanner_t *s, const uint8_t *data, size_t len)
{
    if (!s || (!data && len)) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];

        if (s->collecting) {
            if (c >= '0' && c <= '9') {
                s->digits[s->digit_count++] = (char)c;
                if (s->digit_count == IMEI_SCANNER_MAX_DIGITS) {
                    emit_if_complete(s);
                }
                continue;
            }
            /* Terminó la corrida de dígitos. */
            emit_if_complete(s);
            /* Este byte todavía puede abrir un prefijo nuevo, así que se sigue
             * evaluando abajo en lugar de descartarlo. */
        }

        /* Ventana deslizante con los últimos IMEI_SCANNER_PREFIX_LEN bytes. */
        if (s->window_len < IMEI_SCANNER_PREFIX_LEN) {
            s->window[s->window_len++] = c;
        } else {
            memmove(s->window, s->window + 1, IMEI_SCANNER_PREFIX_LEN - 1);
            s->window[IMEI_SCANNER_PREFIX_LEN - 1] = c;
        }

        if (s->window_len == IMEI_SCANNER_PREFIX_LEN &&
            memcmp(s->window, PREFIX, IMEI_SCANNER_PREFIX_LEN) == 0) {
            s->collecting = true;
            s->digit_count = 0;
            /* Vaciar la ventana: los bytes del prefijo ya se consumieron y no
             * deben participar de una coincidencia posterior. */
            s->window_len = 0;
        }
    }
}

void imei_scanner_get_stats(const imei_scanner_t *s, imei_scanner_stats_t *out)
{
    if (!s || !out) {
        return;
    }
    *out = s->stats;
}
