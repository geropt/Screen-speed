/* Support code for the host shims. */
#include "esp_log.h"

int host_log_level(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("HOST_LOG_LEVEL");
        cached = env ? atoi(env) : 0;
        if (cached < 0) {
            cached = 0;
        }
    }
    return cached;
}
