#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Lightweight diagnostic logger that appends to /sdcard/diag.log.
//
// Purpose: investigate spontaneous power-offs of the device in the vehicle.
// On every boot it records the reset reason (POWERON / BROWNOUT / PANIC / WDT...)
// and, if the previous boot crashed, dumps the core-dump backtrace to the SD card.
// A periodic heartbeat (see diag_log_line) lets us see how long the device ran
// before it died.
//
// Must be called only AFTER the SD card is mounted (sd_card_init()).
void diag_log_init(void);

// Append one printf-formatted line to /sdcard/diag.log. Each line is prefixed
// with the uptime and (once known) the GPS wall-clock time, then flushed and
// fsync'd so it survives an abrupt power cut. Thread-safe.
void diag_log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Provide the latest GPS date/time so subsequent log lines can be anchored to a
// real wall-clock time (the board has no RTC at boot). Safe to call repeatedly.
void diag_log_set_walltime(int year, int month, int day,
                           int hour, int minute, int second);

#ifdef __cplusplus
}
#endif
