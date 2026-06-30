#include "pmu_monitor.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// READ-ONLY telemetry of the AXP2101 PMU to diagnose spontaneous power-offs.
//
// The board (Waveshare ESP32-S3-Touch-AMOLED-1.75) powers everything through the
// AXP2101. The firmware never configures it, so we don't know WHY it cuts power
// (the user has to press the PWR button to bring it back). This module only READS:
// it logs VBUS (the 5V input from the Ruptela), the system rail, battery, the PMU
// die temperature and status bytes on every heartbeat. The last values written to
// /sdcard/diag.log before a power-off reveal the cause: a VBUS dropout points to
// the supply, a die temperature climbing toward ~135 °C points to PMU over-temp.
//
// The ONLY register written is the ADC-enable register (0x30), and only to turn ON
// the measurement channels so the voltages can be read. No power rail, charger,
// PWRKEY or shutdown behavior is touched.

static const char *TAG = "PMU";

// AXP2101 7-bit I2C address (fixed on this PMU).
#define AXP2101_ADDR            0x34
// The PMU sits on the same physical bus as the (disabled) touch controller:
// SDA=15/SCL=14. The LCD port only installs that bus under #if USE_TOUCH, which is
// off, so nothing brings it up — we install it ourselves on I2C_NUM_0 (free, since
// the touch driver never runs). We use the LEGACY driver (driver/i2c.h) on purpose:
// mixing it with the new driver/i2c_master.h links both into the binary and the
// legacy driver's global constructor aborts at boot (i2c: CONFLICT driver_ng).
#define PMU_I2C_PORT            I2C_NUM_0
#define PMU_SDA_GPIO            15
#define PMU_SCL_GPIO            14
#define PMU_I2C_HZ              100000
#define PMU_I2C_TIMEOUT_MS      50

// Register map (standard AXP2101, matches XPowersLib).
#define REG_STATUS1             0x00
#define REG_STATUS2             0x01
#define REG_ADC_CH_CTRL         0x30   // ADC channel enable
#define REG_ADC_VBAT_H          0x34   // vbat   (H5L8)
#define REG_ADC_VBUS_H          0x38   // vbus   (H6L8)
#define REG_ADC_VSYS_H          0x3A   // vsys   (H6L8)
#define REG_ADC_TDIE_H          0x3C   // die T  (H6L8)
#define REG_IRQ_STATUS1         0x48
#define REG_IRQ_STATUS2         0x49
#define REG_IRQ_STATUS3         0x4A

// ADC-enable bits in 0x30: vbat(0) | vbus(2) | vsys(3) | tdie(4) = 0x1D.
#define ADC_ENABLE_MASK         0x1D

static bool s_ready = false;

static esp_err_t pmu_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(PMU_I2C_PORT, AXP2101_ADDR, &reg, 1, val, 1,
                                        pdMS_TO_TICKS(PMU_I2C_TIMEOUT_MS));
}

static esp_err_t pmu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(PMU_I2C_PORT, AXP2101_ADDR, buf, sizeof(buf),
                                      pdMS_TO_TICKS(PMU_I2C_TIMEOUT_MS));
}

// Read a high/low ADC pair and combine. high_mask keeps the valid bits of the
// high byte (0x1F for 13-bit channels, 0x3F for 14-bit). Result is in mV for the
// voltage channels (1 mV/LSB on the AXP2101).
static bool pmu_read_pair(uint8_t reg_h, uint8_t high_mask, uint16_t *out)
{
    uint8_t h, l;
    if (pmu_read_reg(reg_h, &h) != ESP_OK || pmu_read_reg(reg_h + 1, &l) != ESP_OK)
        return false;
    *out = ((uint16_t)(h & high_mask) << 8) | l;
    return true;
}

bool pmu_monitor_init(void)
{
    // Install the legacy I2C master on PMU_I2C_PORT (nothing else uses it: the LCD
    // port's i2c_init() is compiled out by USE_TOUCH=0). Idempotent-ish: if the
    // driver is somehow already installed, i2c_driver_install returns an error and
    // we just proceed to the probe.
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PMU_SDA_GPIO,
        .scl_io_num = PMU_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PMU_I2C_HZ,
    };
    esp_err_t err = i2c_param_config(PMU_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(PMU_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = already installed
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    // Probe: does the PMU answer at 0x34 on these pins?
    uint8_t st1;
    if (pmu_read_reg(REG_STATUS1, &st1) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding at 0x%02X on SDA=%d/SCL=%d",
                 AXP2101_ADDR, PMU_SDA_GPIO, PMU_SCL_GPIO);
        return false;
    }

    // Enable the ADC measurement channels (read-modify-write: don't disturb other
    // bits). This is the ONLY write this module performs.
    uint8_t adc;
    if (pmu_read_reg(REG_ADC_CH_CTRL, &adc) == ESP_OK) {
        uint8_t want = adc | ADC_ENABLE_MASK;
        if (want != adc)
            pmu_write_reg(REG_ADC_CH_CTRL, want);
    }

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 monitor ready (status1=0x%02X)", st1);
    return true;
}

void pmu_monitor_read(pmu_telemetry_t *out)
{
    if (!out)
        return;

    // Safe defaults so a failed read is unambiguous in the log.
    out->ok = false;
    out->vbus_mv = -1;
    out->vsys_mv = -1;
    out->vbat_mv = -1;
    out->die_c = -999;
    out->status1 = 0;
    out->status2 = 0;
    out->irq[0] = out->irq[1] = out->irq[2] = 0;

    if (!s_ready)
        return;

    uint8_t st1, st2;
    if (pmu_read_reg(REG_STATUS1, &st1) != ESP_OK)
        return;     // PMU not answering this cycle
    pmu_read_reg(REG_STATUS2, &st2);

    uint16_t vbus = 0, vsys = 0, vbat = 0, tdie = 0;
    pmu_read_pair(REG_ADC_VBUS_H, 0x3F, &vbus);
    pmu_read_pair(REG_ADC_VSYS_H, 0x3F, &vsys);
    pmu_read_pair(REG_ADC_VBAT_H, 0x1F, &vbat);
    pmu_read_pair(REG_ADC_TDIE_H, 0x3F, &tdie);

    pmu_read_reg(REG_IRQ_STATUS1, &out->irq[0]);
    pmu_read_reg(REG_IRQ_STATUS2, &out->irq[1]);
    pmu_read_reg(REG_IRQ_STATUS3, &out->irq[2]);

    out->status1 = st1;
    out->status2 = st2;
    out->vbus_mv = vbus;
    out->vsys_mv = vsys;
    out->vbat_mv = vbat;
    // Die temperature conversion (approx, per AXP2101/XPowersLib): T = 22 + (7274 - raw)/20.
    // Absolute value may be a few degrees off; the trend toward ~135 °C is what matters.
    out->die_c = (int)(22.0f + (7274.0f - (float)tdie) / 20.0f);
    out->ok = true;
}
