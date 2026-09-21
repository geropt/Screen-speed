/* Host stand-in for esp_system.h. Empty on purpose: sources that need real
 * system services (restart, MAC, reset reason) cannot be characterized on the
 * host and must be exercised on the device. */
#pragma once

#include "esp_err.h"
