#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read-only telemetry snapshot from the AXP2101 PMU.
typedef struct {
    bool    ok;        // true if the I2C read succeeded this call
    int     vbus_mv;   // VBUS (5V input from the Ruptela) in mV, -1 if N/A
    int     vsys_mv;   // system rail voltage in mV
    int     vbat_mv;   // battery voltage in mV (0 if no battery connected)
    int     die_c;     // PMU die temperature in °C (over-temp -> auto power-off)
    uint8_t status1;   // PMU_STATUS1 (0x00): VBUS good / battery present bits
    uint8_t status2;   // PMU_STATUS2 (0x01): charger state bits
    uint8_t irq[3];    // IRQ status 0x48/0x49/0x4A (raw; events since last clear)
} pmu_telemetry_t;

// Bring up I2C0 and the AXP2101 for READ-ONLY monitoring. The only write performed
// is enabling the ADC measurement channels (register 0x30) so voltages can be read;
// no power rail, no PWRKEY behavior, nothing else is touched. Safe to call once
// after the LCD/I2C pins are free. Returns true if the PMU answered on I2C.
bool pmu_monitor_init(void);

// Fill *out with the current PMU telemetry. out->ok is false (and the rest left at
// safe defaults) if the I2C read failed. Thread-safe enough for the calc task use.
void pmu_monitor_read(pmu_telemetry_t *out);

#ifdef __cplusplus
}
#endif
