#include "msp_sched.h"
#include "msp.h"
#include <string.h>

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
} entry_t;

static entry_t s_tbl[] = {
    { MSP_API_VERSION,   30000, 0, 0, 0 },   // once + every 30 s
    { MSP_FC_VARIANT,        0, 0, 0, 0 },
    { MSP_BOXIDS,            0, 0, 0, 0 },
    { MSP_OSD_CANVAS,        0, 0, 0, 0 },
    { MSP2_GET_TEXT,         0, 0, 0, 0 },   // craft name (type 2)
    { MSP_STATUS,           50, 0, 0, 0 },   // 20 Hz
    { MSP_RC,              100, 0, 0, 0 },   // 10 Hz
    { MSP_BATTERY_STATE,  500, 0, 0, 0 },    // 2 Hz
    { MSP_ANALOG,         500, 0, 0, 0 },    // 2 Hz
    { MSP_RAW_GPS,        500, 1, 0, 0 },    // 2 Hz, gps-gated
    { MSP_RTC,          15000, 0, 0, 0 },    // every 15 s, always
};
#define N (sizeof(s_tbl) / sizeof(s_tbl[0]))

void msp_sched_init(void)
{
    for (size_t i = 0; i < N; i++) { s_tbl[i].next_due = 0; s_tbl[i].oneshot_done = 0; }
}

void msp_sched_rearm_oneshots(void)
{
    for (size_t i = 0; i < N; i++) {
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

void msp_sched_mark_oneshot_done(uint16_t func)
{
    for (size_t i = 0; i < N; i++) {
        if (s_tbl[i].func == func && s_tbl[i].period_ms == 0) { s_tbl[i].oneshot_done = 1; }
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
        if (s_tbl[i].period_ms == 0) {
            s_tbl[i].next_due = now + 1000;   // retry the one-shot in 1 s if no resp marks it done
        } else {
            s_tbl[i].next_due = now + s_tbl[i].period_ms;
        }
        return;
    }
}
