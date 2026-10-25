// evlog.c -- fixed-slot event ring + ESP_LOG text mirror. Writers are the
// NimBLE host task, the GATT worker and the HTTP handlers; one spinlock-guarded
// critical section per push, so it is safe from any task (never from an ISR).
#include "evlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EV_SLOTS 64              // ~17 KB; the UI drains it every 200 ms

static ev_t        s_ring[EV_SLOTS];
static uint32_t    s_next = 1;   // seq of the next entry; 0 is "from the start"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void evlog_init(void) { memset(s_ring, 0, sizeof(s_ring)); }

void evlog_push(ev_type_t t, uint16_t handle, int16_t status, uint8_t flags,
                const void *data, size_t len)
{
    if (len > EV_DATA_MAX) { len = EV_DATA_MAX; flags |= EVF_TRUNC; }
    taskENTER_CRITICAL(&s_mux);
    ev_t *e = &s_ring[s_next % EV_SLOTS];
    e->seq = s_next++;
    e->ms = now_ms();
    e->type = (uint8_t)t;
    e->flags = flags;
    e->handle = handle;
    e->status = status;
    e->len = (uint16_t)len;
    if (len) { memcpy(e->data, data, len); }
    taskEXIT_CRITICAL(&s_mux);
}

void evlog_text(ev_type_t t, const char *fmt, ...)
{
    char buf[EV_DATA_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { return; }
    if (n >= (int)sizeof(buf)) { n = sizeof(buf) - 1; }
    ESP_LOGI("ev", "%s", buf);
    evlog_push(t, 0, 0, 0, buf, (size_t)n);
}

bool evlog_get(uint32_t since, ev_t *out)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    uint32_t oldest = s_next > EV_SLOTS ? s_next - EV_SLOTS : 1;
    if (since < oldest) { since = oldest; }
    if (since < s_next) {
        *out = s_ring[since % EV_SLOTS];
        ok = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

uint32_t evlog_next_seq(void) { return s_next; }
uint32_t evlog_oldest_seq(void) { return s_next > EV_SLOTS ? s_next - EV_SLOTS : 1; }

// ---- ESP_LOG mirror ------------------------------------------------------
#define SYSLOG_BYTES 4096
static char     s_sys[SYSLOG_BYTES];
static size_t   s_sys_head;          // next write offset
static bool     s_sys_wrapped;
static vprintf_like_t s_prev_vprintf;

// This hook runs on the stack of EVERY task that logs, including small IDF
// system tasks. It must cost no more than the default vprintf: format ONCE,
// into a static buffer (never a stack buffer, never a second vsnprintf), then
// print that. A second formatting pass here overflowed a 2 KB task at boot.
static char          s_line[256];
static volatile bool s_line_busy;

static int syslog_vprintf(const char *fmt, va_list ap)
{
    bool mine = false;
    taskENTER_CRITICAL(&s_mux);
    if (!s_line_busy) { s_line_busy = mine = true; }
    taskEXIT_CRITICAL(&s_mux);
    if (!mine) {                 // another task is mid-line: print, skip the mirror
        return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    }
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(s_line, sizeof(s_line), fmt, cp);
    va_end(cp);
    int ret;
    if (n >= 0 && n < (int)sizeof(s_line)) {
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < n; i++) {
            s_sys[s_sys_head++] = s_line[i];
            if (s_sys_head == SYSLOG_BYTES) { s_sys_head = 0; s_sys_wrapped = true; }
        }
        taskEXIT_CRITICAL(&s_mux);
        ret = fputs(s_line, stdout) < 0 ? -1 : n;
    } else {                     // too long for the mirror: console only, untruncated
        ret = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    }
    s_line_busy = false;
    return ret;
}

void syslog_hook_init(void) { s_prev_vprintf = esp_log_set_vprintf(syslog_vprintf); }

size_t syslog_dump(char *dst, size_t cap)
{
    size_t n = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_sys_wrapped) {
        size_t tail = SYSLOG_BYTES - s_sys_head;
        size_t a = tail < cap ? tail : cap;
        memcpy(dst, s_sys + s_sys_head, a);
        n = a;
    }
    size_t b = s_sys_head < cap - n ? s_sys_head : cap - n;
    memcpy(dst + n, s_sys, b);
    n += b;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
