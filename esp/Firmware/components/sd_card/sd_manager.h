/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/sdmmc_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNT_POINT "/sdcard"

#define SD_MOSI_GPIO        GPIO_NUM_1
#define SD_SCK_GPIO         GPIO_NUM_2
#define SD_MISO_GPIO        GPIO_NUM_3
#define SD_CS_GPIO          GPIO_NUM_41

typedef struct {
    const char** names;
    const int* pins;
} pin_configuration_t;


esp_err_t sd_card_init();
esp_err_t sd_card_deinit();
void adjust_card_speed(sdmmc_card_t *card, sdmmc_host_t *host);

void check_sd_card_pins(pin_configuration_t *config, const int pin_count);

#ifdef __cplusplus
}
#endif
