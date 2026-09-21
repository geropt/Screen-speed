#include "ruptela_io_parser.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool values[16];
    size_t count;
} ignition_events_t;

static void on_ignition(bool ignition_on, void *user_ctx)
{
    ignition_events_t *events = user_ctx;
    assert(events->count < sizeof(events->values) / sizeof(events->values[0]));
    events->values[events->count++] = ignition_on;
}

static uint8_t test_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? (uint8_t)((crc >> 1) ^ 0xE0U)
                             : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

static size_t make_ignition_frame(uint8_t *frame, uint8_t value)
{
    uint8_t record[32] = {0};
    record[0] = 0x65; /* plausible timestamp; header contents are opaque here */
    record[1] = 0x53;
    record[2] = 0xF1;
    record[3] = 0x00;
    record[5] = 0x00; /* one record, index zero */

    size_t pos = 25;
    record[pos++] = 1;    /* one 1-byte IO */
    record[pos++] = 0x01; /* IO 409, big endian */
    record[pos++] = 0x99;
    record[pos++] = value;
    record[pos++] = 0; /* 2-byte IO count */
    record[pos++] = 0; /* 4-byte IO count */
    record[pos++] = 0; /* 8-byte IO count */
    assert(pos == sizeof(record));

    frame[0] = sizeof(record);
    memcpy(frame + 1, record, sizeof(record));
    frame[1 + sizeof(record)] = test_crc8(record, sizeof(record));
    return sizeof(record) + 2;
}

static size_t decode_hex(const char *hex, uint8_t *out, size_t capacity)
{
    size_t count = 0;
    while (*hex != '\0') {
        while (*hex == ' ') {
            ++hex;
        }
        if (*hex == '\0') {
            break;
        }
        assert(count < capacity);
        char byte_text[3] = {hex[0], hex[1], '\0'};
        out[count++] = (uint8_t)strtoul(byte_text, NULL, 16);
        hex += 2;
    }
    return count;
}

static void test_official_two_record_wire_example(void)
{
    /* Ruptela Protocol 1.133 section 3.2.13.1: two extended records with
     * record-extension markers 0x10 and 0x11. */
    static const char example_hex[] =
        "7B 5B 8F B2 44 00 10 00 0F 08 DF 75 20 A0 C5 2E 09 1F 7E 04 "
        "0D 00 00 0A 00 07 0A 00 05 00 00 1B 1B 00 02 00 00 03 00 00 "
        "1C 01 00 20 1C 00 AD 00 00 73 00 00 CF 00 00 82 00 07 00 1D "
        "3B B5 00 1E 0F EC 00 16 00 0E 00 17 00 0C 00 74 00 00 00 C5 "
        "00 00 00 D2 00 00 06 00 41 00 00 04 53 00 96 00 00 60 1A 00 "
        "5C 00 00 18 06 00 72 00 00 00 D0 00 CB 00 00 00 00 00 D0 00 "
        "00 00 0C 00 75 "
        "53 5B 8F B2 44 00 11 00 0F 08 DF 75 20 A0 C5 2E 09 1F 7E 04 "
        "0D 00 00 0A 00 07 04 00 86 00 00 87 00 00 88 00 00 24 00 09 "
        "00 83 00 00 00 84 00 00 00 85 00 00 00 89 00 00 00 8B 00 00 "
        "02 0F 00 00 02 10 00 00 01 93 00 00 01 92 00 00 01 00 8A 00 "
        "00 00 00 00 2A";

    uint8_t wire[256];
    const size_t wire_len = decode_hex(example_hex, wire, sizeof(wire));
    assert(wire_len == 210);

    ruptela_io_parser_t parser;
    ignition_events_t events = {0};
    ruptela_io_parser_init(&parser, on_ignition, &events);
    ruptela_io_parser_feed(&parser, wire, 17);
    ruptela_io_parser_feed(&parser, wire + 17, wire_len - 17);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(stats.valid_frames == 2);
    assert(stats.malformed_records == 0);
    assert(events.count == 0);
}

static void test_fragmentation_and_repeated_values(void)
{
    ruptela_io_parser_t parser;
    ignition_events_t events = {0};
    uint8_t frame[64];
    size_t frame_len = make_ignition_frame(frame, 1);
    ruptela_io_parser_init(&parser, on_ignition, &events);

    for (size_t i = 0; i < frame_len; ++i) {
        ruptela_io_parser_feed(&parser, frame + i, 1);
    }
    assert(events.count == 1 && events.values[0]);

    ruptela_io_parser_feed(&parser, frame, frame_len);
    frame_len = make_ignition_frame(frame, 0);
    ruptela_io_parser_feed(&parser, frame, frame_len);
    assert(events.count == 3);
    assert(events.values[0] && events.values[1] && !events.values[2]);
}

static void test_noise_bad_crc_and_resynchronization(void)
{
    ruptela_io_parser_t parser;
    ignition_events_t events = {0};
    uint8_t good[64];
    uint8_t bad[64];
    const size_t frame_len = make_ignition_frame(good, 1);
    memcpy(bad, good, frame_len);
    bad[frame_len - 1] ^= 0x5A;

    static const uint8_t noise[] =
        "###IMEI860369052116751\r\n"
        "$GNRMC,161717.00,A,3433.40692,S,05825.44904,W,42.495*00\r\n";

    ruptela_io_parser_init(&parser, on_ignition, &events);
    ruptela_io_parser_feed(&parser, noise, sizeof(noise) - 1);
    ruptela_io_parser_feed(&parser, bad, frame_len);
    ruptela_io_parser_feed(&parser, noise, 11);
    ruptela_io_parser_feed(&parser, good, frame_len);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(events.count == 1 && events.values[0]);
    assert(stats.valid_frames >= 1);
    assert(stats.crc_errors >= 1);
}

static void test_malformed_ignition_is_ignored(void)
{
    ruptela_io_parser_t parser;
    ignition_events_t events = {0};
    uint8_t frame[64];
    const size_t frame_len = make_ignition_frame(frame, 2);
    ruptela_io_parser_init(&parser, on_ignition, &events);
    ruptela_io_parser_feed(&parser, frame, frame_len);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(events.count == 0);
    assert(stats.valid_frames == 1);
    assert(stats.malformed_records == 1);
}

static size_t make_gprs_frame(uint8_t *frame, uint8_t value)
{
    uint8_t record[32] = {0};
    record[0] = 0x65;
    record[1] = 0x53;
    record[2] = 0xF1;
    record[3] = 0x00;
    record[5] = 0x00;

    size_t pos = 25;
    record[pos++] = 1;
    record[pos++] = 0x01;
    record[pos++] = 0xA2;
    record[pos++] = value;
    record[pos++] = 0;
    record[pos++] = 0;
    record[pos++] = 0;
    assert(pos == sizeof(record));

    frame[0] = sizeof(record);
    memcpy(frame + 1, record, sizeof(record));
    frame[1 + sizeof(record)] = test_crc8(record, sizeof(record));
    return sizeof(record) + 2;
}

typedef struct {
    uint8_t records[4][RUPTELA_IO_MAX_RECORD_SIZE];
    uint8_t lengths[4];
    bool gprs[4];
    size_t record_count;
    size_t gprs_count;
} capture_t;

static void on_record(const uint8_t *record, size_t record_len, void *user_ctx)
{
    capture_t *cap = user_ctx;
    assert(cap->record_count < 4);
    memcpy(cap->records[cap->record_count], record, record_len);
    cap->lengths[cap->record_count] = (uint8_t)record_len;
    cap->record_count++;
}

static void on_gprs(bool gprs_up, void *user_ctx)
{
    capture_t *cap = user_ctx;
    cap->gprs[cap->gprs_count++] = gprs_up;
}

static void test_gprs_and_raw_record_callbacks(void)
{
    ruptela_io_parser_t parser;
    capture_t cap = {0};
    uint8_t frame[64];
    size_t frame_len = make_gprs_frame(frame, 1);

    ruptela_io_parser_init(&parser, NULL, &cap);
    ruptela_io_parser_set_gprs_callback(&parser, on_gprs);
    ruptela_io_parser_set_record_callback(&parser, on_record);
    ruptela_io_parser_feed(&parser, frame, frame_len);

    assert(cap.gprs_count == 1 && cap.gprs[0]);
    assert(cap.record_count == 1);
    assert(cap.lengths[0] == 32);

    frame_len = make_gprs_frame(frame, 0);
    ruptela_io_parser_feed(&parser, frame, frame_len);
    assert(cap.gprs_count == 2 && !cap.gprs[1]);
    assert(cap.record_count == 2);
}

static void test_crc_collision_does_not_eat_real_frame(void)
{
    uint8_t fake[34];
    memset(fake, 0, sizeof(fake));
    fake[0] = 32;
    fake[1 + 5] = 0x99;
    fake[33] = test_crc8(fake + 1, 32);

    uint8_t good[64];
    const size_t good_len = make_ignition_frame(good, 1);

    uint8_t buf[128];
    memcpy(buf, fake, sizeof(fake));
    memcpy(buf + sizeof(fake), good, good_len);

    ruptela_io_parser_t parser;
    ignition_events_t events = {0};
    ruptela_io_parser_init(&parser, on_ignition, &events);
    ruptela_io_parser_feed(&parser, buf, sizeof(fake) + good_len);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(events.count == 1 && events.values[0]);
    assert(stats.valid_frames == 1);
}

static size_t wrap_record(uint8_t *frame, const uint8_t *rec, size_t n)
{
    assert(n <= 255);
    frame[0] = (uint8_t)n;
    memcpy(frame + 1, rec, n);
    frame[1 + n] = test_crc8(rec, n);
    return n + 2;
}

static void test_flespi_nmea_payloads_are_not_records(void)
{
    /* Captured on flespi #8913278: ESP wrapped ASCII NMEA as cmd 68. */
    static const uint8_t nmea_imei[] =
        "60369051112793\r\n###IMEI860369051112793\r\n$GNRMC,131339.10";
    static const uint8_t gngns[] =
        "GNS,131338.10,3429.70314,S,05832.88587,W,AANNNN,03,14.83,81.3,13.9,,,V*10\r\n###";

    ruptela_io_parser_t parser;
    capture_t cap = {0};
    uint8_t frame[256];

    ruptela_io_parser_init(&parser, NULL, &cap);
    ruptela_io_parser_set_gprs_callback(&parser, on_gprs);
    ruptela_io_parser_set_record_callback(&parser, on_record);

    size_t n = wrap_record(frame, nmea_imei, sizeof(nmea_imei) - 1);
    ruptela_io_parser_feed(&parser, frame, n);
    n = wrap_record(frame, gngns, sizeof(gngns) - 1);
    ruptela_io_parser_feed(&parser, frame, n);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(cap.record_count == 0);
    assert(cap.gprs_count == 0);
    assert(stats.valid_frames == 0);
    assert(!ruptela_io_record_can_tx(nmea_imei, sizeof(nmea_imei) - 1));
    assert(!ruptela_io_record_can_tx(gngns, sizeof(gngns) - 1));
}

static void test_can_tx_accepts_synthetic_and_official(void)
{
    uint8_t frame[64];
    size_t frame_len = make_gprs_frame(frame, 1);
    assert(ruptela_io_record_can_tx(frame + 1, frame[0]));

    uint8_t wire[256];
    static const char example_hex[] =
        "7B 5B 8F B2 44 00 10 00 0F 08 DF 75 20 A0 C5 2E 09 1F 7E 04 "
        "0D 00 00 0A 00 07 0A 00 05 00 00 1B 1B 00 02 00 00 03 00 00 "
        "1C 01 00 20 1C 00 AD 00 00 73 00 00 CF 00 00 82 00 07 00 1D "
        "3B B5 00 1E 0F EC 00 16 00 0E 00 17 00 0C 00 74 00 00 00 C5 "
        "00 00 00 D2 00 00 06 00 41 00 00 04 53 00 96 00 00 60 1A 00 "
        "5C 00 00 18 06 00 72 00 00 00 D0 00 CB 00 00 00 00 00 D0 00 "
        "00 00 0C 00 75";
    const size_t wire_len = decode_hex(example_hex, wire, sizeof(wire));
    assert(wire[0] == 0x7B);
    assert(ruptela_io_record_can_tx(wire + 1, wire[0]));
    (void)frame_len;
    (void)wire_len;
}


/* Un record real cuyo CRC no coincide tiene que contarse aparte de las posiciones de
 * byte que el barrido prueba y descarta.
 *
 * Motivo, medido sobre tres capturas de campo: `crc_errors` cuenta cada posición
 * probada, así que sobre un enlace que además transporta NMEA dio 110 229, 92 801 y
 * 84 721. Como señal de salud no sirve. `frames_crc_failed` cuenta sólo lo que pasa la
 * validación estructural completa y falla CRC8, y sobre esas mismas capturas dio 9, 8
 * y 10: la cantidad real de records que llegaron dañados. */
static void test_frames_crc_failed_cuenta_records_reales(void)
{
    uint8_t frame[RUPTELA_IO_FRAME_BUFFER_SIZE];
    size_t frame_len = make_ignition_frame(frame, 1);

    /* Corromper un byte del interior del record deja la estructura intacta y rompe el
     * CRC: es exactamente el caso de campo. */
    frame[frame_len - 6] ^= 0xFF;

    ignition_events_t events = {0};
    ruptela_io_parser_t parser;
    ruptela_io_parser_init(&parser, on_ignition, &events);
    ruptela_io_parser_feed(&parser, frame, frame_len);

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(stats.valid_frames == 0);
    assert(events.count == 0);
    /* Lo importante: se reporta como record dañado, no como ruido. */
    assert(stats.frames_crc_failed == 1);
}

static void test_ruido_no_cuenta_como_record_danado(void)
{
    /* Texto NMEA puro: muchas posiciones probadas, ningún record dañado. Si esto
     * contara, el indicador volvería a ser inútil. */
    const char *nmea =
        "$GNRMC,160927.70,A,3432.04083,S,05830.20690,W,59.745,150.08,050826,,,A,V*3C\r\n"
        "$GNGNS,160927.70,3432.04083,S,05830.20690,W,AANNNN,12,0.95,25.1,13.8,,,V*27\r\n"
        "###IMEI860369052116751";

    ignition_events_t events = {0};
    ruptela_io_parser_t parser;
    ruptela_io_parser_init(&parser, on_ignition, &events);
    for (int rep = 0; rep < 20; ++rep) {
        ruptela_io_parser_feed(&parser, (const uint8_t *)nmea, strlen(nmea));
    }

    ruptela_io_parser_stats_t stats;
    ruptela_io_parser_get_stats(&parser, &stats);
    assert(stats.valid_frames == 0);
    assert(stats.frames_crc_failed == 0);
}

int main(void)
{
    test_official_two_record_wire_example();
    test_fragmentation_and_repeated_values();
    test_noise_bad_crc_and_resynchronization();
    test_malformed_ignition_is_ignored();
    test_gprs_and_raw_record_callbacks();
    test_crc_collision_does_not_eat_real_frame();
    test_flespi_nmea_payloads_are_not_records();
    test_can_tx_accepts_synthetic_and_official();
    test_frames_crc_failed_cuenta_records_reales();
    test_ruido_no_cuenta_como_record_danado();
    puts("ruptela_io_parser tests passed");
    return 0;
}
