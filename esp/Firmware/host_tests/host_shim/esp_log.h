/* Host stand-in for esp_log.h.
 *
 * Only exists so ESP-IDF components can be compiled and exercised on a
 * developer machine (see host_tests/README.md). Level is controlled with the
 * HOST_LOG_LEVEL environment variable (0 = silent, default; 5 = verbose) so a
 * test run stays quiet unless something is being debugged.
 *
 * The macros reference `tag` on purpose: several sources declare a file-scope
 * `static const char *TAG` that would otherwise trip -Wunused-variable under
 * -Werror.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 none, 1 error, 2 warn, 3 info, 4 debug, 5 verbose */
int host_log_level(void);

#define HOST_LOG_AT(level, letter, tag, fmt, ...)                            \
    do {                                                                     \
        if (host_log_level() >= (level)) {                                    \
            fprintf(stderr, "%c (%s) " fmt "\n", letter, (tag), ##__VA_ARGS__); \
        } else {                                                              \
            (void)(tag);                                                      \
        }                                                                     \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) HOST_LOG_AT(1, 'E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG_AT(2, 'W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG_AT(3, 'I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG_AT(4, 'D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) HOST_LOG_AT(5, 'V', tag, fmt, ##__VA_ARGS__)

#define esp_log_level_set(tag, level) do { (void)(tag); (void)(level); } while (0)

#ifdef __cplusplus
}
#endif
