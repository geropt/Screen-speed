#include "ruptela_io_parser.h"

#include <string.h>

/* Extended records have a 25-byte header and four one-byte group counts. */
#define RUPTELA_EXTENDED_HEADER_SIZE 25U
#define RUPTELA_MIN_EXTENDED_RECORD_SIZE (RUPTELA_EXTENDED_HEADER_SIZE + 4U)

static uint8_t ruptela_crc8(const uint8_t *data, size_t len)
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

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

/* Unix 2000-01-01 .. 2050-01-01. NMEA ASCII collisions land outside this. */
#define RUPTELA_TS_MIN 946684800U
#define RUPTELA_TS_MAX 2524608000U

static bool plausible_gnss_header(const uint8_t *record, size_t record_len)
{
    int32_t lon;
    int32_t lat;

    if (record_len < RUPTELA_MIN_EXTENDED_RECORD_SIZE ||
        record_len > RUPTELA_EXTENDED_RECORD_MAX) {
        return false;
    }

    const uint32_t ts = read_be32(record);
    if (ts < RUPTELA_TS_MIN || ts > RUPTELA_TS_MAX) {
        return false;
    }

    lon = (int32_t)read_be32(record + 7U);
    lat = (int32_t)read_be32(record + 11U);
    if (lon < -1800000000 || lon > 1800000000) {
        return false;
    }
    if (lat < -900000000 || lat > 900000000) {
        return false;
    }
    return true;
}

/* 1 = ok, 0 = structure ok but ignition value unusable, -1 = not a record. */
static int validate_extended_record(const uint8_t *record, size_t record_len)
{
    if (record_len < RUPTELA_MIN_EXTENDED_RECORD_SIZE) {
        return -1;
    }

    const uint8_t record_extension = record[5];
    const uint8_t total_index = (uint8_t)(record_extension >> 4);
    const uint8_t record_index = (uint8_t)(record_extension & 0x0FU);
    if (total_index > 7U || record_index > total_index) {
        return -1;
    }

    size_t pos = RUPTELA_EXTENDED_HEADER_SIZE;
    bool ignition_malformed = false;
    static const uint8_t value_sizes[] = {1U, 2U, 4U, 8U};

    for (size_t group = 0; group < sizeof(value_sizes); ++group) {
        if (pos >= record_len) {
            return -1;
        }

        const uint8_t count = record[pos++];
        const size_t item_size = 2U + value_sizes[group];
        if ((size_t)count > (record_len - pos) / item_size) {
            return -1;
        }

        for (uint8_t item = 0; item < count; ++item) {
            const uint16_t io_id = read_be16(record + pos);
            pos += 2U;
            if (io_id == RUPTELA_IO_CUSTOM_IGNITION_ID) {
                if (value_sizes[group] != 1U || record[pos] > 1U) {
                    ignition_malformed = true;
                }
            }
            pos += value_sizes[group];
        }
    }

    if (pos != record_len) {
        return -1;
    }
    if (!plausible_gnss_header(record, record_len)) {
        return -1;
    }
    return ignition_malformed ? 0 : 1;
}

static void emit_found_ios(ruptela_io_parser_t *parser,
                           bool ignition_found, bool ignition_on,
                           bool gprs_found, bool gprs_up)
{
    if (ignition_found && parser->ignition_callback != NULL) {
        parser->ignition_callback(ignition_on, parser->user_ctx);
    }
    if (gprs_found && parser->gprs_callback != NULL) {
        parser->gprs_callback(gprs_up, parser->user_ctx);
    }
}

static void discard_prefix(ruptela_io_parser_t *parser, size_t count)
{
    if (count >= parser->buffered) {
        parser->buffered = 0;
        return;
    }

    parser->buffered -= count;
    memmove(parser->buffer, parser->buffer + count, parser->buffered);
}

static int parse_extended_record(ruptela_io_parser_t *parser,
                                 const uint8_t *record,
                                 size_t record_len)
{
    const int validated = validate_extended_record(record, record_len);
    if (validated < 0) {
        return -1;
    }

    size_t pos = RUPTELA_EXTENDED_HEADER_SIZE;
    bool ignition_found = false;
    bool ignition_on = false;
    bool gprs_found = false;
    bool gprs_up = false;
    static const uint8_t value_sizes[] = {1U, 2U, 4U, 8U};

    for (size_t group = 0; group < sizeof(value_sizes); ++group) {
        const uint8_t count = record[pos++];
        for (uint8_t item = 0; item < count; ++item) {
            const uint16_t io_id = read_be16(record + pos);
            pos += 2U;
            if (io_id == RUPTELA_IO_CUSTOM_IGNITION_ID &&
                value_sizes[group] == 1U && record[pos] <= 1U) {
                ignition_found = true;
                ignition_on = record[pos] == 1U;
            } else if (io_id == RUPTELA_IO_GPRS_ID) {
                gprs_found = true;
                if (value_sizes[group] == 1U) {
                    gprs_up = record[pos] != 0U;
                } else {
                    bool nonzero = false;
                    for (uint8_t b = 0; b < value_sizes[group]; ++b) {
                        if (record[pos + b] != 0U) {
                            nonzero = true;
                            break;
                        }
                    }
                    gprs_up = nonzero;
                }
            }
            pos += value_sizes[group];
        }
    }

    emit_found_ios(parser, ignition_found, ignition_on, gprs_found, gprs_up);
    return validated;
}

static void process_buffer(ruptela_io_parser_t *parser)
{
    while (parser->buffered > 0U) {
        bool consumed_frame = false;

        /* Search the whole buffered window. A plausible but incomplete length
         * byte in NMEA/noise must not hide a later complete Ruptela frame. */
        for (size_t start = 0; start < parser->buffered; ++start) {
            const size_t record_len = parser->buffer[start];
            if (record_len < RUPTELA_MIN_EXTENDED_RECORD_SIZE ||
                record_len > RUPTELA_IO_MAX_RECORD_SIZE) {
                continue;
            }

            const size_t frame_len = 1U + record_len + 1U;
            if (parser->buffered - start < frame_len) {
                continue;
            }

            const uint8_t expected_crc = parser->buffer[start + 1U + record_len];
            const uint8_t actual_crc = ruptela_crc8(parser->buffer + start + 1U,
                                                    record_len);
            if (actual_crc != expected_crc) {
                continue;
            }

            /* CRC8 is only 8 bits: NMEA often collides. Do not consume a
             * collision — that would eat the real grouped record (409/418). */
            const int parsed = parse_extended_record(parser,
                                                     parser->buffer + start + 1U,
                                                     record_len);
            if (parsed < 0) {
                continue;
            }

            ++parser->stats.valid_frames;
            if (parsed == 0) {
                ++parser->stats.malformed_records;
            }
            if (parser->record_callback != NULL) {
                parser->record_callback(parser->buffer + start + 1U, record_len,
                                        parser->user_ctx);
            }
            discard_prefix(parser, start + frame_len);
            consumed_frame = true;
            break;
        }

        if (consumed_frame) {
            continue;
        }

        const size_t first_record_len = parser->buffer[0];
        if (first_record_len < RUPTELA_MIN_EXTENDED_RECORD_SIZE ||
            first_record_len > RUPTELA_IO_MAX_RECORD_SIZE) {
            discard_prefix(parser, 1U);
            continue;
        }

        const size_t first_frame_len = 1U + first_record_len + 1U;
        if (parser->buffered >= first_frame_len) {
            ++parser->stats.crc_errors;
            /* The first complete candidate failed CRC. Advance one byte; the
             * full-window scan above already protected any later valid frame. */
            discard_prefix(parser, 1U);
            continue;
        }

        return;
    }
}

void ruptela_io_parser_init(ruptela_io_parser_t *parser,
                            ruptela_ignition_callback_t ignition_callback,
                            void *user_ctx)
{
    if (parser == NULL) {
        return;
    }

    memset(parser, 0, sizeof(*parser));
    parser->ignition_callback = ignition_callback;
    parser->gprs_callback = NULL;
    parser->record_callback = NULL;
    parser->user_ctx = user_ctx;
}

void ruptela_io_parser_set_gprs_callback(ruptela_io_parser_t *parser,
                                         ruptela_gprs_callback_t gprs_callback)
{
    if (parser != NULL) {
        parser->gprs_callback = gprs_callback;
    }
}

void ruptela_io_parser_set_record_callback(ruptela_io_parser_t *parser,
                                           ruptela_record_callback_t record_callback)
{
    if (parser != NULL) {
        parser->record_callback = record_callback;
    }
}

void ruptela_io_parser_reset(ruptela_io_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->buffered = 0U;
    memset(&parser->stats, 0, sizeof(parser->stats));
}

void ruptela_io_parser_feed(ruptela_io_parser_t *parser,
                            const uint8_t *data,
                            size_t len)
{
    if (parser == NULL || data == NULL) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        if (parser->buffered == sizeof(parser->buffer)) {
            /* A full buffer always contains either a complete maximum-size
             * candidate or noise. Give it one last chance, then advance. */
            process_buffer(parser);
            if (parser->buffered == sizeof(parser->buffer)) {
                discard_prefix(parser, 1U);
            }
        }

        parser->buffer[parser->buffered++] = data[i];
        process_buffer(parser);
    }
}

void ruptela_io_parser_get_stats(const ruptela_io_parser_t *parser,
                                 ruptela_io_parser_stats_t *stats)
{
    if (parser == NULL || stats == NULL) {
        return;
    }

    *stats = parser->stats;
}

bool ruptela_io_record_can_tx(const uint8_t *record, size_t record_len)
{
    if (record == NULL) {
        return false;
    }
    return validate_extended_record(record, record_len) >= 0;
}
