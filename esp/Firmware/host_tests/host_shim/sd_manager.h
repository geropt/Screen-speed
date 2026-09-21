/* Host stand-in for components/sd_card/sd_manager.h.
 *
 * Shadows the device header so `tile_reader.h` resolves without pulling in the
 * SDMMC driver, and points MOUNT_POINT at a directory on the host instead of
 * "/sdcard". TILE_PATH is built by string-literal concatenation in
 * tile_reader.h, so HOST_MOUNT_POINT must expand to a literal; the Makefile
 * passes it as an absolute path.
 *
 * The SD lifecycle functions are intentionally NOT declared: mounting,
 * remount and card-removal behaviour cannot be characterized here and belong to
 * on-device tests.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifndef HOST_MOUNT_POINT
#error "HOST_MOUNT_POINT must be defined by the host_tests Makefile"
#endif

#define MOUNT_POINT HOST_MOUNT_POINT
