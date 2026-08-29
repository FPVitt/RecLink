// applog.h -- 4 KB line ring buffer mirrored from the ESP log (spec 12.6).
// Exposed to the portal via GET /api/logs. Lock-free single-writer friendly:
// writes happen from many tasks so a spinlock guards the append.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define APPLOG_RING_BYTES 4096

// Install as the esp_log vprintf hook. Call once, early in app_main.
void applog_init(void);

// Copy the most recent bytes (up to buf_len-1) into buf, NUL-terminated.
// Returns bytes written. Oldest partial line is trimmed to a clean boundary.
size_t applog_dump(char *buf, size_t buf_len);

// Structured one-liner for error counters surfaced in /api/status.errors.
void applog_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
