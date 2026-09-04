#include "ruptela_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_heartbeat_vector(void)
{
    const uint64_t imei = 869530043817416ULL;
    uint8_t pkt[16];
    size_t n = ruptela_build_heartbeat(pkt, sizeof(pkt), imei);
    static const uint8_t expect[] = {
        0x00, 0x09, 0x00, 0x03, 0x16, 0xd5, 0x3d, 0x62, 0x1d, 0xc8, 0x10, 0x50, 0xa6
    };
    assert(n == sizeof(expect));
    assert(memcmp(pkt, expect, sizeof(expect)) == 0);
}

static void test_ack_parse(void)
{
    static const uint8_t ack[] = {0x00, 0x02, 0x64, 0x01, 0x13, 0xbc};
    ruptela_server_msg_t msg;
    size_t used = ruptela_parse_server_packet(ack, sizeof(ack), &msg);
    assert(used == sizeof(ack));
    assert(msg.cmd == RUPTELA_CMD_ACK_RECORDS);
    assert(msg.payload_len == 1);
    assert(msg.payload[0] == 0x01);

    static const uint8_t hb_ack[] = {0x00, 0x02, 0x74, 0x01, 0x86, 0x2d};
    used = ruptela_parse_server_packet(hb_ack, sizeof(hb_ack), &msg);
    assert(used == sizeof(hb_ack));
    assert(msg.cmd == RUPTELA_CMD_ACK_HEARTBEAT);
}

static void test_cmd68_crc_roundtrip(void)
{
    const uint64_t imei = 869530043817416ULL;
    uint8_t rec[29] = {0};
    rec[5] = 0x00;
    rec[25] = 0;
    rec[26] = 0;
    rec[27] = 0;
    rec[28] = 0;
    const uint8_t *recs[] = { rec };
    size_t lens[] = { sizeof(rec) };
    uint8_t pkt[128];
    size_t n = ruptela_build_cmd68(pkt, sizeof(pkt), imei, recs, lens, 1, 0);
    assert(n > 13);
    assert(pkt[10] == RUPTELA_CMD_RECORDS_EXT);
    uint16_t crc = ((uint16_t)pkt[n - 2] << 8) | pkt[n - 1];
    assert(crc == ruptela_crc16(pkt + 2, n - 4));
}

static void test_drop_list(void)
{
    assert(ruptela_cmd_must_drop(RUPTELA_CMD_CFG));
    assert(ruptela_cmd_must_drop(RUPTELA_CMD_FOTA));
    assert(ruptela_cmd_must_drop(RUPTELA_CMD_SMS_GPRS));
    assert(ruptela_cmd_must_drop(RUPTELA_CMD_SET_IO));
    assert(!ruptela_cmd_must_drop(RUPTELA_CMD_ACK_RECORDS));
}

int main(void)
{
    test_heartbeat_vector();
    test_ack_parse();
    test_cmd68_crc_roundtrip();
    test_drop_list();
    puts("ruptela_proto tests passed");
    return 0;
}
