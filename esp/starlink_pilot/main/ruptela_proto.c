#include "ruptela_proto.h"

#include <string.h>

uint16_t ruptela_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            uint16_t carry = (uint16_t)(crc & 1U);
            crc >>= 1;
            if (carry) {
                crc ^= 0x8408U;
            }
        }
    }
    return crc;
}

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; --i) {
        p[7 - i] = (uint8_t)(v >> (i * 8));
    }
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

size_t ruptela_build_device_packet(uint8_t *out, size_t cap, uint64_t imei,
                                   uint8_t cmd, const uint8_t *payload,
                                   size_t payload_len)
{
    if (out == NULL) {
        return 0;
    }
    const size_t body = 8U + 1U + payload_len;
    const size_t total = 2U + body + 2U;
    if (total > cap || total > RUPTELA_MAX_PACKET || body > 0xFFFFU) {
        return 0;
    }
    if (payload_len > 0 && payload == NULL) {
        return 0;
    }

    write_be16(out, (uint16_t)body);
    write_be64(out + 2, imei);
    out[10] = cmd;
    if (payload_len > 0) {
        memcpy(out + 11, payload, payload_len);
    }
    uint16_t crc = ruptela_crc16(out + 2, body);
    write_be16(out + 2 + body, crc);
    return total;
}

size_t ruptela_build_heartbeat(uint8_t *out, size_t cap, uint64_t imei)
{
    return ruptela_build_device_packet(out, cap, imei, RUPTELA_CMD_HEARTBEAT, NULL, 0);
}

size_t ruptela_build_cmd68(uint8_t *out, size_t cap, uint64_t imei,
                           const uint8_t *const *records, const size_t *record_lens,
                           size_t nrecords, uint8_t records_left)
{
    if (nrecords == 0 || nrecords > RUPTELA_MAX_RECORDS_PER_PACKET ||
        records == NULL || record_lens == NULL) {
        return 0;
    }

    uint8_t payload[RUPTELA_MAX_PACKET];
    size_t pos = 0;
    payload[pos++] = records_left;
    payload[pos++] = (uint8_t)nrecords;
    for (size_t i = 0; i < nrecords; ++i) {
        if (records[i] == NULL || record_lens[i] == 0 ||
            pos + record_lens[i] > sizeof(payload)) {
            return 0;
        }
        memcpy(payload + pos, records[i], record_lens[i]);
        pos += record_lens[i];
    }
    return ruptela_build_device_packet(out, cap, imei, RUPTELA_CMD_RECORDS_EXT,
                                       payload, pos);
}

size_t ruptela_parse_server_packet(const uint8_t *data, size_t len,
                                   ruptela_server_msg_t *msg)
{
    if (data == NULL || msg == NULL || len < 5U) {
        return 0;
    }
    uint16_t body = read_be16(data);
    size_t total = 2U + (size_t)body + 2U;
    if (body < 1U || total > len || total > RUPTELA_MAX_PACKET) {
        return 0;
    }
    uint16_t got = read_be16(data + 2U + body);
    if (ruptela_crc16(data + 2, body) != got) {
        return 0;
    }
    msg->cmd = data[2];
    msg->payload = (body > 1U) ? (data + 3) : NULL;
    msg->payload_len = (body > 1U) ? (size_t)(body - 1U) : 0U;
    return total;
}

bool ruptela_cmd_must_drop(uint8_t cmd)
{
    return cmd == RUPTELA_CMD_CFG || cmd == RUPTELA_CMD_FOTA ||
           cmd == RUPTELA_CMD_SMS_GPRS || cmd == RUPTELA_CMD_SET_IO;
}
