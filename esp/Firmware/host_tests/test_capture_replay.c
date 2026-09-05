/* Replay de una captura real del enlace del Ruptela.
 *
 * Alimenta los tres consumidores del framing compartido —framer de sentencias, scanner
 * de IMEI y parser de records de IO— con un stream reconstruido de una captura de
 * campo, en trozos de tamaño variable para imitar las lecturas de UART.
 *
 * Esto es la validación que faltaba: hasta acá el framing se probó con vectores
 * sintéticos y con la matriz de fallas, pero nunca contra bytes que salieron de un
 * tracker de verdad.
 *
 * Uso: test_capture_replay <archivo.bin> [tamaño de trozo]
 *
 * Con 0 como tamaño de trozo, recorre varios tamaños —incluido 1 byte— y verifica que
 * el resultado sea idéntico en todos. Esa invariancia es la propiedad que importa: el
 * troceo de la UART no coincide con las fronteras de los mensajes, así que ningún
 * resultado puede depender de dónde cayó el corte.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "nmea_framer.h"
#include "imei_scanner.h"
#include "ruptela_io_parser.h"

typedef struct {
    uint32_t sentences;
    uint32_t rmc;
    uint32_t gns;
    uint32_t other;
    uint32_t imeis;
    char     last_imei[IMEI_SCANNER_MAX_DIGITS + 1];
    uint32_t imei_changes;
    uint32_t records;
    uint32_t ignition_events;
    uint32_t gprs_events;
    bool     last_ignition;
    bool     last_gprs;
    size_t   record_bytes;
} tally_t;

static void on_sentence(void *ctx, const char *s, size_t len)
{
    tally_t *t = (tally_t *)ctx;
    (void)len;
    t->sentences++;
    /* El talker es GN (multiconstelación), no GP: el matcher del HUD identifica por
     * "RMC" con strstr, así que esto le llega igual. */
    if (strstr(s, "RMC")) {
        t->rmc++;
    } else if (strstr(s, "GNS")) {
        t->gns++;
    } else {
        t->other++;
    }
}

static void on_imei(void *ctx, const char *imei, size_t len)
{
    tally_t *t = (tally_t *)ctx;
    (void)len;
    t->imeis++;
    if (t->last_imei[0] && strcmp(t->last_imei, imei) != 0) {
        t->imei_changes++;
    }
    snprintf(t->last_imei, sizeof(t->last_imei), "%s", imei);
}

static void on_ignition(bool on, void *ctx)
{
    tally_t *t = (tally_t *)ctx;
    t->ignition_events++;
    t->last_ignition = on;
}

static void on_gprs(bool up, void *ctx)
{
    tally_t *t = (tally_t *)ctx;
    t->gprs_events++;
    t->last_gprs = up;
}

static void on_record(const uint8_t *rec, size_t len, void *ctx)
{
    tally_t *t = (tally_t *)ctx;
    (void)rec;
    t->records++;
    t->record_bytes += len;
}

/* Corre el stream completo con un tamaño de trozo dado. */
static void replay(const uint8_t *data, size_t len, size_t chunk,
                  tally_t *out, nmea_framer_stats_t *fstats,
                  ruptela_io_parser_stats_t *iostats)
{
    memset(out, 0, sizeof(*out));

    nmea_framer_t framer;
    nmea_framer_init(&framer, on_sentence, out);
    imei_scanner_t scanner;
    imei_scanner_init(&scanner, on_imei, out);
    ruptela_io_parser_t io;
    ruptela_io_parser_init(&io, on_ignition, out);
    ruptela_io_parser_set_gprs_callback(&io, on_gprs);
    ruptela_io_parser_set_record_callback(&io, on_record);

    size_t off = 0;
    while (off < len) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        nmea_framer_feed(&framer, data + off, n);
        imei_scanner_feed(&scanner, data + off, n);
        ruptela_io_parser_feed(&io, data + off, n);
        off += n;
    }

    nmea_framer_get_stats(&framer, fstats);
    ruptela_io_parser_get_stats(&io, iostats);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: %s <archivo.bin> [tamaño de trozo]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "no se pudo abrir %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "no se pudo leer %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    size_t requested = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10) : 0;

    /* Tamaños de trozo a probar. 1 byte es el caso extremo: el corte cae en cada
     * posición posible del stream. 64 y 1024 imitan lecturas reales de UART. */
    const size_t chunks[] = { 1, 7, 64, 512, 1024, (size_t)size };
    const size_t nchunks = sizeof(chunks) / sizeof(chunks[0]);

    tally_t base;
    nmea_framer_stats_t base_f;
    ruptela_io_parser_stats_t base_io;

    if (requested) {
        replay(data, (size_t)size, requested, &base, &base_f, &base_io);
    } else {
        replay(data, (size_t)size, chunks[0], &base, &base_f, &base_io);
        /* Invariancia: el resultado no puede depender del troceo. */
        for (size_t i = 1; i < nchunks; ++i) {
            tally_t t;
            nmea_framer_stats_t fs;
            ruptela_io_parser_stats_t ios;
            replay(data, (size_t)size, chunks[i], &t, &fs, &ios);
            if (t.sentences != base.sentences || t.imeis != base.imeis ||
                t.records != base.records || t.ignition_events != base.ignition_events ||
                t.gprs_events != base.gprs_events ||
                fs.crc_errors != base_f.crc_errors ||
                ios.valid_frames != base_io.valid_frames) {
                fprintf(stderr,
                        "DIFERENCIA con trozo de %zu bytes contra el de %zu:\n"
                        "  sentencias %u/%u imei %u/%u records %u/%u\n",
                        chunks[i], chunks[0],
                        t.sentences, base.sentences, t.imeis, base.imeis,
                        t.records, base.records);
                free(data);
                return 1;
            }
        }
    }

    printf("captura: %s (%ld bytes)\n", argv[1], size);
    printf("  NMEA      sentencias=%u (RMC=%u GNS=%u otras=%u)\n",
           base.sentences, base.rmc, base.gns, base.other);
    printf("            crc_malo=%u abortadas_control=%u reinicio_$=%u overflow=%u\n",
           base_f.crc_errors, base_f.aborted_control, base_f.aborted_restart,
           base_f.overflow);
    printf("            checksum_no_hex=%u ruido=%u\n",
           base_f.bad_checksum_char, base_f.noise_bytes);
    printf("  IMEI      marcadores=%u ultimo=%s cambios=%u\n",
           base.imeis, base.last_imei[0] ? base.last_imei : "(ninguno)",
           base.imei_changes);
    printf("  IO        records=%u (%zu bytes) validos=%u crc_malo=%u malformados=%u\n",
           base.records, base.record_bytes, base_io.valid_frames,
           base_io.crc_errors, base_io.malformed_records);
    printf("            ignicion=%u (ultima=%d) gprs=%u (ultimo=%d)\n",
           base.ignition_events, (int)base.last_ignition,
           base.gprs_events, (int)base.last_gprs);
    if (!requested) {
        printf("  invariancia: identico con trozos de 1, 7, 64, 512, 1024 y %ld bytes\n",
               size);
    }

    free(data);
    return 0;
}
