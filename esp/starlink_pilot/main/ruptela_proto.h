#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUPTELA_CMD_RECORDS_EXT 0x44U
#define RUPTELA_CMD_HEARTBEAT 0x10U
#define RUPTELA_CMD_ACK_RECORDS 0x64U
#define RUPTELA_CMD_ACK_HEARTBEAT 0x74U
#define RUPTELA_CMD_CFG 102U
#define RUPTELA_CMD_FOTA 104U
#define RUPTELA_CMD_SMS_GPRS 108U
#define RUPTELA_CMD_SET_IO 117U

#define RUPTELA_MAX_PACKET 1024U
#define RUPTELA_MAX_RECORDS_PER_PACKET 8U

uint16_t ruptela_crc16(const uint8_t *data, size_t len);

/** Device→server packet: length + IMEI + cmd + payload + CRC16. */
size_t ruptela_build_device_packet(uint8_t *out, size_t cap, uint64_t imei,
                                   uint8_t cmd, const uint8_t *payload,
                                   size_t payload_len);

size_t ruptela_build_heartbeat(uint8_t *out, size_t cap, uint64_t imei);

size_t ruptela_build_cmd68(uint8_t *out, size_t cap, uint64_t imei,
                           const uint8_t *const *records, const size_t *record_lens,
                           size_t nrecords, uint8_t records_left);

typedef struct {
    uint8_t cmd;
    const uint8_t *payload;
    size_t payload_len;
} ruptela_server_msg_t;

/** Parse one server packet (no IMEI). Returns bytes consumed, or 0 if incomplete/bad. */
size_t ruptela_parse_server_packet(const uint8_t *data, size_t len,
                                   ruptela_server_msg_t *msg);

bool ruptela_cmd_must_drop(uint8_t cmd);

#ifdef __cplusplus
}
#endif
