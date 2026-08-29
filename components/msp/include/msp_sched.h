// msp_sched.h -- round-robin MSP request cadence (spec 4.2). Never floods: at
// most one request emitted per scheduler tick, chosen by earliest due time.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct msp_sched msp_sched_t;

void msp_sched_init(void);

// Pick the next request to send now. Returns false if nothing is due.
// gps_present gates the GPS/analog/battery polls (spec 4.2 note).
bool msp_sched_pick(uint32_t now_ms, bool gps_present, bool osd_supported,
                    uint16_t *out_func);

// Mark a func as just-sent (advances its next-due by its period).
void msp_sched_sent(uint16_t func, uint32_t now_ms);

// Re-arm the one-shot requests (api/variant/boxids/canvas/craft) -- called at
// boot and on FC link recovery (spec 4.2 "once + on reconnect").
void msp_sched_rearm_oneshots(void);

// True once every one-shot has completed at least once.
bool msp_sched_oneshots_done(void);
void msp_sched_mark_oneshot_done(uint16_t func);
