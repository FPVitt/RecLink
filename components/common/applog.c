#include "applog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char     s_ring[APPLOG_RING_BYTES];
static uint32_t s_head;        // next write index
static bool     s_wrapped;
static vprintf_like_t s_prev;  // chain to the original (UART/USB) sink

static void ring_put(const char *s, int n)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = s[i];
        if (++s_head >= APPLOG_RING_BYTES) { s_head = 0; s_wrapped = true; }
    }
    portEXIT_CRITICAL(&s_mux);
}

static int applog_vprintf(const char *fmt, va_list ap)
{
    char line[192];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        ring_put(line, n > (int)sizeof(line) ? (int)sizeof(line) : n);
    }
    return s_prev ? s_prev(fmt, ap) : n;
}

void applog_init(void)
{
    s_prev = esp_log_set_vprintf(applog_vprintf);
}

size_t applog_dump(char *buf, size_t buf_len)
{
    if (buf_len == 0) { return 0; }
    portENTER_CRITICAL(&s_mux);
    size_t avail = s_wrapped ? APPLOG_RING_BYTES : s_head;
    size_t want  = avail < buf_len - 1 ? avail : buf_len - 1;
    size_t start = (s_head + APPLOG_RING_BYTES - want) % APPLOG_RING_BYTES;
    for (size_t i = 0; i < want; i++) {
        buf[i] = s_ring[(start + i) % APPLOG_RING_BYTES];
    }
    buf[want] = '\0';
    portEXIT_CRITICAL(&s_mux);

    // Trim a leading partial line for cleanliness.
    char *nl = memchr(buf, '\n', want);
    if (nl && want == buf_len - 1) {
        size_t off = (size_t)(nl - buf) + 1;
        memmove(buf, buf + off, want - off + 1);
        return want - off;
    }
    return want;
}

void applog_note(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ESP_LOGW("note", "%s", line);
    }
}
