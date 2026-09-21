#include "board_i2c.h"
#include "board_waveshare_175.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "BOARD_I2C";

static bool s_ready;
static SemaphoreHandle_t s_lock;
static uint32_t s_probes, s_absent, s_bus_errs;

static bool lock_take(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return false;
        }
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t board_i2c_acquire(void)
{
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_ready) {
        lock_give();
        return ESP_OK;
    }

    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = BOARD_PIN_I2C_SDA;
    conf.scl_io_num = BOARD_PIN_I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = BOARD_I2C_FREQ_HZ;

    esp_err_t err = i2c_param_config(BOARD_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config: %s", esp_err_to_name(err));
        lock_give();
        return err;
    }

    err = i2c_driver_install(BOARD_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install: %s", esp_err_to_name(err));
        lock_give();
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "bus listo: puerto %d, SDA GPIO%d, SCL GPIO%d, %d Hz",
             (int)BOARD_I2C_PORT, (int)BOARD_PIN_I2C_SDA, (int)BOARD_PIN_I2C_SCL,
             BOARD_I2C_FREQ_HZ);
    lock_give();
    return ESP_OK;
}

bool board_i2c_is_ready(void)
{
    return s_ready;
}

esp_err_t board_i2c_probe(uint8_t addr_7bit, uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        lock_give();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = i2c_master_start(cmd);
    if (err == ESP_OK) {
        err = i2c_master_write_byte(cmd, (uint8_t)(addr_7bit << 1) | I2C_MASTER_WRITE, true);
    }
    if (err == ESP_OK) {
        err = i2c_master_stop(cmd);
    }
    if (err == ESP_OK) {
        err = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(timeout_ms));
    }
    i2c_cmd_link_delete(cmd);

    s_probes++;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "0x%02x responde", addr_7bit);
    } else if (err == ESP_FAIL) {
        /* Sin ACK: el periférico no está. No es una falla del sistema. */
        s_absent++;
        err = ESP_ERR_NOT_FOUND;
        ESP_LOGW(TAG, "0x%02x ausente", addr_7bit);
    } else {
        s_bus_errs++;
        ESP_LOGE(TAG, "0x%02x error de bus: %s", addr_7bit, esp_err_to_name(err));
    }

    lock_give();
    return err;
}

void board_i2c_stats(uint32_t *out_probes, uint32_t *out_absent,
                     uint32_t *out_bus_errs)
{
    if (out_probes)   *out_probes = s_probes;
    if (out_absent)   *out_absent = s_absent;
    if (out_bus_errs) *out_bus_errs = s_bus_errs;
}

/* Timeout de las transferencias de registro. Corto a propósito: un periférico
 * colgado no puede bloquear a quien lo consulta. */
#define BOARD_I2C_XFER_TIMEOUT_MS 50

int board_i2c_read_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    if (!s_ready || (!data && len)) {
        return -1;
    }
    esp_err_t err = i2c_master_write_read_device(BOARD_I2C_PORT, dev_addr,
                                                &reg_addr, 1, data, len,
                                                pdMS_TO_TICKS(BOARD_I2C_XFER_TIMEOUT_MS));
    if (err != ESP_OK) {
        s_bus_errs++;
        return -1;
    }
    return 0;
}

int board_i2c_write_regs(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    if (!s_ready) {
        return -1;
    }
    /* Un solo write con registro y datos contiguos. El límite mantiene el buffer
     * en la pila y acotado; ningún registro de touch/PMIC/RTC del proyecto
     * escribe más que esto de una vez. */
    enum { MAX_PAYLOAD = 32 };
    if (len > MAX_PAYLOAD) {
        ESP_LOGE(TAG, "escritura de %u B a 0x%02x excede el límite de %d B",
                 (unsigned)len, dev_addr, MAX_PAYLOAD);
        return -1;
    }
    uint8_t buf[1 + MAX_PAYLOAD];
    buf[0] = reg_addr;
    if (len && data) {
        memcpy(&buf[1], data, len);
    }
    esp_err_t err = i2c_master_write_to_device(BOARD_I2C_PORT, dev_addr,
                                              buf, (size_t)len + 1,
                                              pdMS_TO_TICKS(BOARD_I2C_XFER_TIMEOUT_MS));
    if (err != ESP_OK) {
        s_bus_errs++;
        return -1;
    }
    return 0;
}
