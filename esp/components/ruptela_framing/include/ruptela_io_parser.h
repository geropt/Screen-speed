#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUPTELA_IO_CUSTOM_IGNITION_ID 409U
#define RUPTELA_IO_GPRS_ID 418U
#define RUPTELA_IO_MAX_RECORD_SIZE 255U
#define RUPTELA_EXTENDED_RECORD_MAX 126U
#define RUPTELA_IO_FRAME_BUFFER_SIZE (1U + RUPTELA_IO_MAX_RECORD_SIZE + 1U)

typedef void (*ruptela_ignition_callback_t)(bool ignition_on, void *user_ctx);
typedef void (*ruptela_gprs_callback_t)(bool gprs_up, void *user_ctx);
typedef void (*ruptela_record_callback_t)(const uint8_t *record, size_t record_len,
                                         void *user_ctx);

typedef struct {
    uint32_t valid_frames;
    uint32_t crc_errors;
    uint32_t malformed_records;
} ruptela_io_parser_stats_t;

typedef struct {
    uint8_t buffer[RUPTELA_IO_FRAME_BUFFER_SIZE];
    size_t buffered;
    ruptela_ignition_callback_t ignition_callback;
    ruptela_gprs_callback_t gprs_callback;
    ruptela_record_callback_t record_callback;
    void *user_ctx;
    ruptela_io_parser_stats_t stats;
} ruptela_io_parser_t;

/**
 * @brief Initialize a streaming parser for Ruptela RS232 I/O records.
 *
 * The wire format is: record length (1B), record data (length bytes), CRC8
 * (1B). The parser tolerates arbitrary non-Ruptela bytes around records and
 * keeps partial frames between calls.
 */
void ruptela_io_parser_init(ruptela_io_parser_t *parser,
                            ruptela_ignition_callback_t ignition_callback,
                            void *user_ctx);

void ruptela_io_parser_set_gprs_callback(ruptela_io_parser_t *parser,
                                         ruptela_gprs_callback_t gprs_callback);

void ruptela_io_parser_set_record_callback(ruptela_io_parser_t *parser,
                                           ruptela_record_callback_t record_callback);

/** Reset buffered wire data and counters without changing the callback. */
void ruptela_io_parser_reset(ruptela_io_parser_t *parser);

/** Feed the next raw UART bytes into the parser. */
void ruptela_io_parser_feed(ruptela_io_parser_t *parser,
                            const uint8_t *data,
                            size_t len);

/** Copy diagnostic counters collected since initialization/reset. */
void ruptela_io_parser_get_stats(const ruptela_io_parser_t *parser,
                                 ruptela_io_parser_stats_t *stats);

/**
 * True if the blob is a complete extended record (IO groups consume exactly
 * `record_len`, GNSS header looks like protocol §2, size ≤ 126). Use this
 * before wrapping command 68: CRC8 collisions on NMEA must not go on the wire.
 */
bool ruptela_io_record_can_tx(const uint8_t *record, size_t record_len);

#ifdef __cplusplus
}
#endif
