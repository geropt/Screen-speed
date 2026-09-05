/* Vectores independientes del protocolo Ruptela.
 *
 * Cierra el punto ciego que quedó registrado en P02a: el test existente de `cmd68`
 * construye el paquete con `ruptela_build_cmd68()` y lo verifica con
 * `ruptela_crc16()`, o sea que valida la función contra sí misma. Un roundtrip así
 * pasa igual si el layout está equivocado de punta a punta.
 *
 * Acá los bytes esperados se escriben a mano, derivados de la especificación, y se
 * comparan contra la salida del encoder. Si el layout cambia, esto falla; si el
 * encoder y su propio CRC cambian juntos, esto también falla, que es justo lo que un
 * roundtrip no detecta.
 *
 * Aclaración de nomenclatura, registrada en P02a: el «cmd68» del plan es
 * RUPTELA_CMD_RECORDS_EXT = 0x44, porque **68 es decimal**. En hexadecimal 0x68 = 104
 * = RUPTELA_CMD_FOTA, que está en la lista de comandos a descartar.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ruptela_proto.h"

/* IMEI de prueba, el mismo que usa el vector dorado del heartbeat. */
#define TEST_IMEI 356938035643809ULL

/* CRC-16 reimplementado a partir de la especificación, INDEPENDIENTE del de
 * producción: reflejado, init 0, constante XOR 0x8408 (CCITT/Kermit reflejado).
 * Que dos implementaciones escritas por separado coincidan es evidencia; que una se
 * verifique consigo misma no lo es. */
static uint16_t crc16_reference(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1) {
                crc = (uint16_t)((crc >> 1) ^ 0x8408U);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void dump(const char *label, const uint8_t *b, size_t n)
{
    fprintf(stderr, "%s (%zu B):", label, n);
    for (size_t i = 0; i < n; ++i) {
        fprintf(stderr, " %02X", b[i]);
    }
    fprintf(stderr, "\n");
}

static void expect_equal(const char *what, const uint8_t *got, size_t got_len,
                        const uint8_t *want, size_t want_len)
{
    if (got_len != want_len || memcmp(got, want, want_len) != 0) {
        fprintf(stderr, "DIFERENCIA en %s\n", what);
        dump("  esperado", want, want_len);
        dump("  obtenido", got, got_len);
        assert(0);
    }
}

/* ---------- estructura del paquete de dispositivo ----------
 *
 * Derivada de la especificación:
 *   [0..1]        longitud del cuerpo, big endian
 *   [2..9]        IMEI, big endian de 64 bits
 *   [10]          comando
 *   [11..]        payload
 *   [2+body..+1]  CRC16 big endian, calculado sobre IMEI+cmd+payload
 *                 (NO incluye el prefijo de longitud)
 *   total = 2 + body + 2
 */

static void test_heartbeat_vector_independiente(void)
{
    /* Construido a mano byte por byte. body = 8 (IMEI) + 1 (cmd) + 0 = 9. */
    uint8_t want[13];
    size_t n = 0;
    want[n++] = 0x00;
    want[n++] = 0x09;                      /* longitud del cuerpo = 9        */
    /* IMEI 356938035643809 = 0x001450A9C2A0E1 en 64 bits big endian. */
    uint64_t imei = TEST_IMEI;
    for (int i = 7; i >= 0; --i) {
        want[n++] = (uint8_t)((imei >> (8 * i)) & 0xFF);
    }
    want[n++] = 0x10;                      /* RUPTELA_CMD_HEARTBEAT          */
    uint16_t crc = crc16_reference(&want[2], 9);
    want[n++] = (uint8_t)(crc >> 8);
    want[n++] = (uint8_t)(crc & 0xFF);
    assert(n == 13);

    uint8_t got[64];
    size_t got_len = ruptela_build_heartbeat(got, sizeof(got), TEST_IMEI);
    expect_equal("heartbeat", got, got_len, want, n);
}

static void test_cmd68_vector_independiente(void)
{
    /* EL vector que faltaba. Un record sintético de 12 bytes, y el paquete armado
     * a mano según la especificación.
     *
     * Payload de records extendidos: [cantidad de records][records...]. */
    uint8_t record[12];
    for (size_t i = 0; i < sizeof(record); ++i) {
        record[i] = (uint8_t)(0x10 + i);
    }

    /* payload = [records_left][cantidad de records][records...] */
    const size_t payload_len = 2 + sizeof(record);
    const size_t body = 8 + 1 + payload_len;

    uint8_t want[64];
    size_t n = 0;
    want[n++] = (uint8_t)((body >> 8) & 0xFF);
    want[n++] = (uint8_t)(body & 0xFF);
    uint64_t imei = TEST_IMEI;
    for (int i = 7; i >= 0; --i) {
        want[n++] = (uint8_t)((imei >> (8 * i)) & 0xFF);
    }
    want[n++] = 0x44;                     /* RECORDS_EXT: 68 decimal        */
    want[n++] = 0x00;                     /* records_left = 0               */
    want[n++] = 0x01;                     /* un record                      */
    memcpy(&want[n], record, sizeof(record));
    n += sizeof(record);
    uint16_t crc = crc16_reference(&want[2], body);
    want[n++] = (uint8_t)(crc >> 8);
    want[n++] = (uint8_t)(crc & 0xFF);

    const uint8_t *records[1] = { record };
    size_t lengths[1] = { sizeof(record) };
    uint8_t got[128];
    size_t got_len = ruptela_build_cmd68(got, sizeof(got), TEST_IMEI,
                                        records, lengths, 1, 0);
    expect_equal("cmd68 con un record", got, got_len, want, n);

    /* Y el comando es 0x44, no 0x68: dejarlo aseverado evita que alguien
     * "corrija" el valor creyendo que el plan pide hexadecimal. */
    assert(got[10] == 0x44);
    assert(got[10] != 0x68);
}

static void test_cmd68_multiples_records(void)
{
    /* El test anterior del repositorio sólo cubría un record. Con dos se verifica
     * que la cantidad y la concatenación son correctas. */
    uint8_t r1[4] = { 0xA0, 0xA1, 0xA2, 0xA3 };
    uint8_t r2[6] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5 };

    const size_t payload_len = 2 + sizeof(r1) + sizeof(r2);
    const size_t body = 8 + 1 + payload_len;

    uint8_t want[64];
    size_t n = 0;
    want[n++] = (uint8_t)((body >> 8) & 0xFF);
    want[n++] = (uint8_t)(body & 0xFF);
    uint64_t imei = TEST_IMEI;
    for (int i = 7; i >= 0; --i) {
        want[n++] = (uint8_t)((imei >> (8 * i)) & 0xFF);
    }
    want[n++] = 0x44;
    want[n++] = 0x03;                     /* records_left = 3               */
    want[n++] = 0x02;                     /* dos records en este paquete    */
    memcpy(&want[n], r1, sizeof(r1)); n += sizeof(r1);
    memcpy(&want[n], r2, sizeof(r2)); n += sizeof(r2);
    uint16_t crc = crc16_reference(&want[2], body);
    want[n++] = (uint8_t)(crc >> 8);
    want[n++] = (uint8_t)(crc & 0xFF);

    const uint8_t *records[2] = { r1, r2 };
    size_t lengths[2] = { sizeof(r1), sizeof(r2) };
    uint8_t got[128];
    size_t got_len = ruptela_build_cmd68(got, sizeof(got), TEST_IMEI,
                                        records, lengths, 2, 3);
    expect_equal("cmd68 con dos records", got, got_len, want, n);
}

static void test_crc16_coincide_con_la_referencia(void)
{
    /* Dos implementaciones escritas por separado, sobre varios vectores. */
    const char *vectors[] = { "", "A", "123456789", "\xFF\x00\xFF\x00", "Ruptela" };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const uint8_t *b = (const uint8_t *)vectors[i];
        size_t len = strlen(vectors[i]);
        assert(ruptela_crc16(b, len) == crc16_reference(b, len));
    }
    /* Y un caso con NUL intercalado, que un strlen no cubriría. */
    const uint8_t with_nul[] = { 0x01, 0x00, 0x02, 0x00, 0x03 };
    assert(ruptela_crc16(with_nul, sizeof(with_nul)) ==
           crc16_reference(with_nul, sizeof(with_nul)));
}

static void test_limites_de_buffer(void)
{
    /* Un buffer que no alcanza no puede producir un paquete a medias. */
    uint8_t tiny[4];
    assert(ruptela_build_heartbeat(tiny, sizeof(tiny), TEST_IMEI) == 0);

    uint8_t record[12] = {0};
    const uint8_t *records[1] = { record };
    size_t lengths[1] = { sizeof(record) };
    assert(ruptela_build_cmd68(tiny, sizeof(tiny), TEST_IMEI, records, lengths, 1, 0) == 0);

    /* Buffer exactamente justo sí funciona. */
    uint8_t exact[13];
    assert(ruptela_build_heartbeat(exact, sizeof(exact), TEST_IMEI) == 13);

    /* Cero records se rechaza en lugar de emitir un paquete vacío. */
    uint8_t big[128];
    assert(ruptela_build_cmd68(big, sizeof(big), TEST_IMEI, records, lengths, 0, 0) == 0);
}

static void test_ack_con_crc_malo_se_rechaza(void)
{
    /* El test existente sólo probaba ACKs bien formados. */
    uint8_t pkt[8];
    size_t n = 0;
    pkt[n++] = 0x00;
    pkt[n++] = 0x01;          /* body = 1: sólo el comando */
    pkt[n++] = 0x64;          /* ACK_RECORDS               */
    uint16_t crc = crc16_reference(&pkt[2], 1);
    pkt[n++] = (uint8_t)(crc >> 8);
    pkt[n++] = (uint8_t)(crc & 0xFF);

    ruptela_server_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    assert(ruptela_parse_server_packet(pkt, n, &msg) == n);
    assert(msg.cmd == 0x64);

    /* Corromper el CRC: no puede aceptarse. */
    pkt[n - 1] ^= 0xFF;
    assert(ruptela_parse_server_packet(pkt, n, &msg) == 0);
}

static void test_paquete_de_servidor_incompleto(void)
{
    uint8_t pkt[8];
    size_t n = 0;
    pkt[n++] = 0x00;
    pkt[n++] = 0x01;
    pkt[n++] = 0x64;
    uint16_t crc = crc16_reference(&pkt[2], 1);
    pkt[n++] = (uint8_t)(crc >> 8);
    pkt[n++] = (uint8_t)(crc & 0xFF);

    ruptela_server_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    /* Cortado en cada offset: nunca debe aceptar ni leer fuera de rango. */
    for (size_t cut = 0; cut < n; ++cut) {
        assert(ruptela_parse_server_packet(pkt, cut, &msg) == 0);
    }
}

static void test_lista_de_descarte(void)
{
    /* 0x68 = 104 = FOTA está en la lista de descarte: confirma que el «68» del plan
     * NO puede ser hexadecimal. */
    assert(ruptela_cmd_must_drop(0x68) == true);
    assert(ruptela_cmd_must_drop(0x44) == false);
}

int main(void)
{
    test_heartbeat_vector_independiente();
    test_cmd68_vector_independiente();
    test_cmd68_multiples_records();
    test_crc16_coincide_con_la_referencia();
    test_limites_de_buffer();
    test_ack_con_crc_malo_se_rechaza();
    test_paquete_de_servidor_incompleto();
    test_lista_de_descarte();
    printf("ruptela_proto vectores independientes: tests passed\n");
    return 0;
}
