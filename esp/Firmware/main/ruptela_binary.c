#include "ruptela_binary.h"
#include "vehicle_io.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "RUPT_IO";

// CRC16/IBM (Ruptela): poly 0x8005, reflected → 0xA001, init 0x0000
static uint16_t crc16_ruptela(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

static inline uint16_t rd_u16_be(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

// Parse one IO section group. val_size = bytes per value (1,2,4,8).
// Returns number of bytes consumed, or -1 on truncation.
static int parse_io_section(const uint8_t *p, size_t avail, int val_size)
{
    if (avail < 2) return -1;
    uint16_t count = rd_u16_be(p);
    size_t needed = 2 + (size_t)count * (2 + val_size);
    if (avail < needed) return -1;

    const uint8_t *cur = p + 2;
    for (int i = 0; i < count; i++) {
        uint16_t io_id = rd_u16_be(cur);
        cur += 2;
        uint32_t raw = 0;
        for (int b = 0; b < val_size && b < 4; b++)
            raw = (raw << 8) | cur[b];
        cur += val_size;

        switch (io_id) {
        case 197: set_io_rpm((int32_t)(raw * 125 / 1000)); break;     // ×0.125
        case 206: set_io_accel_pct((int32_t)(raw * 4 / 10)); break;   // ×0.4
        case 39:  set_io_engine_load_pct((int32_t)raw); break;
        case 36:  set_io_brake_active(raw != 0); break;
        case 116: set_io_fuel_rate_x10((int32_t)(raw / 2)); break;        // ×0.05 × stored×10 = ×0.5
        case 137: set_io_idling_sec((int32_t)raw); break;
        case 239: set_io_ignition(raw != 0); break;
        case 240: set_io_moving(raw != 0); break;
        case 66:  set_io_ext_voltage_mv((int32_t)raw); break;
        default:  break;
        }
    }
    return (int)needed;
}

bool ruptela_record_parse(const uint8_t *buf, size_t len)
{
    // Minimum: 4B preamble + 8B IMEI + 1B codec + 1B num_records +
    //          1B num_records_dup + 2B CRC = 17B (plus at least one record)
    if (len < 17) return false;

    // Preamble: 0x00 0x00 [DataLength:2B BE]
    uint16_t data_length = rd_u16_be(buf + 2);
    if ((size_t)(data_length + 6) > len) {
        ESP_LOGW(TAG, "truncated: data_length=%u len=%u", data_length, (unsigned)len);
        return false;
    }

    // CRC covers: IMEI(8) + codec(1) + num_records(1) + records + num_records_dup(1)
    // = bytes [4 .. 4+data_length-1] (data_length includes everything except preamble and CRC)
    // Ruptela CRC position: last 2 bytes of the packet
    uint16_t crc_recv = ((uint16_t)buf[len - 2] << 8) | buf[len - 1];
    uint16_t crc_calc = crc16_ruptela(buf + 4, len - 6); // skip preamble(4) and CRC(2)
    if (crc_recv != crc_calc) {
        ESP_LOGW(TAG, "CRC mismatch: recv=0x%04X calc=0x%04X", crc_recv, crc_calc);
        // Don't return false immediately — first packet after power-on may have
        // framing artifacts. Log and skip.
        return false;
    }

    uint8_t num_records = buf[13];
    const uint8_t *p = buf + 14;  // points to first record
    const uint8_t *end = buf + len - 3; // exclude num_records_dup + CRC

    for (int r = 0; r < num_records; r++) {

        // Record header layout (24 bytes):
        //  [0 -3 ] Timestamp     4B
        //  [4    ] TimestampExt  1B
        //  [5    ] Priority      1B
        //  [6 -9 ] Longitude     4B  signed ×10^-7
        //  [10-13] Latitude      4B  signed ×10^-7
        //  [14-15] Altitude      2B
        //  [16-17] Angle         2B  heading ° ×100
        //  [18   ] Satellites    1B
        //  [19-20] Speed         2B  km/h
        //  [21   ] HDOP          1B
        //  [22-23] EventID       2B
        // NOTE: verify offsets against RUPT_RAW hex dump before trusting values.
        if (p + 24 > end) break;
        uint16_t angle_raw = rd_u16_be(p + 16);
        uint16_t speed_raw = rd_u16_be(p + 19);
        set_io_ruptela_heading((float)angle_raw / 100.0f);
        ESP_LOGD(TAG, "record: heading=%.1f° speed=%u km/h",
                 (float)angle_raw / 100.0f, speed_raw);
        p += 24;

        // 4 IO sections: 1B, 2B, 4B, 8B values
        int val_sizes[] = {1, 2, 4, 8};
        for (int s = 0; s < 4; s++) {
            int consumed = parse_io_section(p, (size_t)(end - p), val_sizes[s]);
            if (consumed < 0) {
                ESP_LOGW(TAG, "IO section %d truncated", s);
                goto done;
            }
            p += consumed;
        }
    }

done:
    ESP_LOGI(TAG, "rpm=%ld accel=%ld%% load=%ld%% ign=%d move=%d vbat=%ldmV idling=%lds",
             (long)get_io_rpm(), (long)get_io_accel_pct(), (long)get_io_engine_load_pct(),
             get_io_ignition(), get_io_moving(),
             (long)get_io_ext_voltage_mv(), (long)get_io_idling_sec());
    return true;
}
