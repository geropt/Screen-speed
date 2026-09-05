/* Host stand-in for esp_vfs_fat.h. The host uses the real filesystem through
 * libc, so nothing is needed beyond letting the include resolve. */
#pragma once

#include "esp_err.h"
