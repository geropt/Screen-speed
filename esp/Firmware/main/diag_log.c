#include "diag_log.h"
#include "sd_manager.h"   // MOUNT_POINT

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

static const char *TAG = "DIAG";

#define DIAG_LOG_PATH   MOUNT_POINT "/diag.log"

static SemaphoreHandle_t s_mutex = NULL;

// Latest GPS wall-clock time, used to anchor log lines (the board has no RTC at
// boot). s_have_time stays false until the first GPS fix calls set_walltime.
static volatile bool s_have_time = false;
static volatile int s_y, s_mo, s_d, s_h, s_mi, s_s;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON:   return "POWERON";   // power applied / cold boot
    case ESP_RST_EXT:       return "EXT";       // external reset pin
    case ESP_RST_SW:        return "SW";         // esp_restart()
    case ESP_RST_PANIC:     return "PANIC";      // exception / abort
    case ESP_RST_INT_WDT:   return "INT_WDT";    // interrupt watchdog
    case ESP_RST_TASK_WDT:  return "TASK_WDT";   // task watchdog
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";   // supply voltage dipped
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

// Write one already-formatted message line to the log file. Opens, writes,
// flushes and fsyncs on every call so nothing is lost on an abrupt power cut.
static void write_line(const char *msg)
{
    FILE *f = fopen(DIAG_LOG_PATH, "a");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "cannot open %s", DIAG_LOG_PATH);
        return;
    }

    int64_t up_ms = esp_timer_get_time() / 1000;
    if (s_have_time)
        fprintf(f, "[up=%lldms %04d-%02d-%02d %02d:%02d:%02d] %s\n",
                up_ms, s_y, s_mo, s_d, s_h, s_mi, s_s, msg);
    else
        fprintf(f, "[up=%lldms] %s\n", up_ms, msg);

    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
// If the previous boot crashed, dump the stored core-dump backtrace to the SD
// card so it can be recovered from the vehicle without a PC/UART attached, then
// erase the image so the partition is free for the next crash.
static void dump_coredump_if_any(void)
{
    if (esp_core_dump_image_check() != ESP_OK)
        return;   // no (valid) core dump stored

    esp_core_dump_summary_t *sum = malloc(sizeof(esp_core_dump_summary_t));
    if (sum != NULL && esp_core_dump_get_summary(sum) == ESP_OK)
    {
        char line[256];
        snprintf(line, sizeof(line),
                 "COREDUMP task=%s exc_pc=0x%08" PRIx32 " depth=%" PRIu32,
                 sum->exc_task, sum->exc_pc, sum->exc_bt_info.depth);
        write_line(line);

        // Backtrace addresses (symbolize later with addr2line / idf monitor).
        char bt[256];
        int n = snprintf(bt, sizeof(bt), "COREDUMP bt:");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth &&
                             i < (sizeof(sum->exc_bt_info.bt) / sizeof(sum->exc_bt_info.bt[0])) &&
                             n < (int)sizeof(bt) - 12; i++)
            n += snprintf(bt + n, sizeof(bt) - n, " 0x%08" PRIx32, sum->exc_bt_info.bt[i]);
        write_line(bt);
    }
    free(sum);

    // Drop the image regardless, so a future crash isn't masked by this one.
    esp_core_dump_image_erase();
}
#endif

void diag_log_init(void)
{
    if (s_mutex == NULL)
        s_mutex = xSemaphoreCreateMutex();

    esp_reset_reason_t reason = esp_reset_reason();

    char banner[160];
    snprintf(banner, sizeof(banner),
             "===== BOOT reason=%s(%d) heap=%u psram=%u =====",
             reset_reason_str(reason), (int)reason,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (s_mutex)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    write_line(banner);
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT)
        dump_coredump_if_any();
#endif
    if (s_mutex)
        xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "diag log ready (%s)", reset_reason_str(reason));
}

void diag_log_line(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (s_mutex)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    write_line(msg);
    if (s_mutex)
        xSemaphoreGive(s_mutex);
}

void diag_log_set_walltime(int year, int month, int day,
                           int hour, int minute, int second)
{
    s_y = year; s_mo = month; s_d = day;
    s_h = hour; s_mi = minute; s_s = second;
    s_have_time = true;
}
