#include "msp_sched.h"
#include "msp.h"
#include <string.h>

// Retry interval for an unanswered one-shot (startup handshake reads). Kept
// short: these requests are a few bytes, harmless to repeat, and the FC may
// still be finishing its own boot when the first one is sent (at ~t=0 after
// the ESP's own boot) -- a 1 s retry meant up to ~1 s of dead air once the FC
// actually became ready, directly adding to how long MSP link-up (and
// everything gated on it) visibly took.
#define ONESHOT_RETRY_MS 150

// period_ms == 0  -> one-shot (fires once until re-armed)
// gps_gated       -> only when fc_state.gps_present
// osd_gated       -> only when osd_supported (skip craft/canvas otherwise? no --
//                    canvas is fine, it's harmless; keep all reads ungated)
typedef struct {
    uint16_t func;
    uint16_t period_ms;
    uint8_t  gps_gated;
    uint32_t next_due;
    uint8_t  oneshot_done;
    // Set the first time ANY response for this func has ever been seen,
    // regardless of period_ms. Until then, every entry -- periodic ones
    // included -- is retried at ONESHOT_RETRY_MS, not its steady-state
    // period: MSP_API_VERSION's 30 s period is fine once it's answered, but
    // using that same 30 s as the RETRY for a first request that got lost
    // (FC not ready yet, a dropped byte) left osd_supported (gated on that
    // response) stuck off for up to 30 s -- the actual "MSP is slow" bug.
    uint8_t  answered_once;
} entry_t;

static entry_t s_tbl[] = {
    { MSP_API_VERSION,   30000, 0, 0, 0, 0 },   // once + every 30 s
    { MSP_FC_VARIANT,        0, 0, 0, 0, 0 },
    { MSP_BOXIDS,            0, 0, 0, 0, 0 },
    { MSP_OSD_CANVAS,        0, 0, 0, 0, 0 },
    { MSP2_GET_TEXT,         0, 0, 0, 0, 0 },   // craft name (type 2)
    { MSP_STATUS,           50, 0, 0, 0, 0 },   // 20 Hz
    { MSP_RC,              100, 0, 0, 0, 0 },   // 10 Hz
    { MSP_BATTERY_STATE,  500, 0, 0, 0, 0 },    // 2 Hz
    { MSP_ANALOG,         500, 0, 0, 0, 0 },    // 2 Hz
    { MSP_RAW_GPS,        500, 1, 0, 0, 0 },    // 2 Hz, gps-gated
    { MSP_RTC,          15000, 0, 0, 0, 0 },    // every 15 s, always
};
#define N (sizeof(s_tbl) / sizeof(s_tbl[0]))

void msp_sched_init(void)
{
    for (size_t i = 0; i < N; i++) {
        s_tbl[i].next_due = 0; s_tbl[i].oneshot_done = 0; s_tbl[i].answered_once = 0;
    }
}

void msp_sched_rearm_oneshots(void)
{
    for (size_t i = 0; i < N; i++) {
        // Re-handshake on recovery (msp_task.c): every entry goes through
        // fast-retry again, not just the period_ms==0 ones -- a reconnect
        // shouldn't leave MSP_API_VERSION waiting out its full 30 s period
        // for its first post-reconnect answer either.
        s_tbl[i].answered_once = 0;
        if (s_tbl[i].period_ms == 0) { s_tbl[i].oneshot_done = 0; s_tbl[i].next_due = 0; }
    }
}

bool msp_sched_oneshots_done(void)
{
    for (size_t i = 0; i < N; i++) {
        if (s_tbl[i].period_ms == 0 && !s_tbl[i].oneshot_done) { return false; }
    }
    return true;
}

void msp_sched_mark_responded(uint16_t func)
{
    for (size_t i = 0; i < N; i++) {
        if (s_tbl[i].func != func) { continue; }
        s_tbl[i].answered_once = 1;
        if (s_tbl[i].period_ms == 0) { s_tbl[i].oneshot_done = 1; }
    }
}

bool msp_sched_pick(uint32_t now, bool gps_present, bool osd_supported, uint16_t *out)
{
    (void)osd_supported;
    int best = -1;
    uint32_t best_late = 0;
    for (size_t i = 0; i < N; i++) {
        entry_t *e = &s_tbl[i];
        if (e->gps_gated && !gps_present) { continue; }
        if (e->period_ms == 0 && e->oneshot_done) { continue; }
        if ((int32_t)(now - e->next_due) < 0) { continue; }   // not due yet
        uint32_t late = now - e->next_due;
        if (best < 0 || late > best_late) { best = (int)i; best_late = late; }
    }
    if (best < 0) { return false; }
    *out = s_tbl[best].func;
    return true;
}

void msp_sched_sent(uint16_t func, uint32_t now)
{
    for (size_t i = 0; i < N; i++) {
        if (s_tbl[i].func != func) { continue; }
        if (s_tbl[i].period_ms == 0 || !s_tbl[i].answered_once) {
            // One-shot, or a periodic entry that has never been answered
            // yet (e.g. MSP_API_VERSION's first request got lost) -- retry
            // fast rather than waiting out its full steady-state period.
            s_tbl[i].next_due = now + ONESHOT_RETRY_MS;
        } else {
            s_tbl[i].next_due = now + s_tbl[i].period_ms;
        }
        return;
    }
}
