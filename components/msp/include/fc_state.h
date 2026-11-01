// fc_state.h -- the FC-side shared model (spec 3.4). Written ONLY by msp_task,
// read by everyone else. Access is copy-in/copy-out under a mutex; callers must
// never hold the mutex across a driver/queue/log call.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bridge_types.h"

typedef struct {
    bool     link_up;               // valid MSP RX within FC_LINK_TIMEOUT_MS
    uint32_t last_rx_ms;

    // Boot-time diagnostics (portal MSP debug panel): uptime (ms) the first
    // time each became true since THIS boot -- never reset on a later
    // reconnect, only on an ESP reboot. 0 = hasn't happened yet this boot.
    uint32_t link_up_first_ms;
    uint32_t osd_first_ms;           // first time osd_supported went true

    uint8_t  api_major, api_minor;
    char     fc_variant[5];         // "BTFL"
    bool     osd_supported;         // api >= 1.47

    bool     armed;
    bool     armed_valid;           // armed was decoded from MSP_STATUS with the arm bit resolved
                                    // since the FC link last came up; false = arm state UNKNOWN
    uint32_t arming_disable_flags;
    int8_t   arm_bit_index;         // BOXARM bit in flightModeFlags (from MSP_BOXIDS); -1 = unresolved

    uint16_t rc[18];                // us; idx 0..3 = RPYT, 4.. = AUX1..
    uint8_t  rc_channel_count;

    // measured MSP receive cadence (EMA of the interval between responses, ms;
    // 0 = not measured yet). Diagnostics only -- surfaced in /api/status.
    uint16_t rc_period_ms;          // MSP_RC  (nominal 100 ms / 10 Hz)
    uint16_t status_period_ms;      // MSP_STATUS (nominal 50 ms / 20 Hz)
    uint32_t rc_last_rx_ms;         // uptime of the last MSP_RC response

    gps_fix_t gps;
    bool     gps_present;           // MSP_STATUS sensor bit 3
    bool     gps_good;              // fix >= 3D && sats >= cfg.clock.min_sats
    uint32_t gps_good_since_ms;     // uptime when gps_good last became true (0 = not)

    int32_t  rtc_epoch;             // Unix UTC seconds from MSP_RTC (0 = unset)
    uint16_t rtc_millis;
    uint32_t rtc_sample_uptime_ms;
    bool     rtc_valid;             // plausible year (> 2025-01-01)

    uint8_t  batt_state;            // 0 OK 1 WARN 2 CRIT 3 NONE
    uint8_t  batt_pct_est;

    uint16_t osd_cols, osd_rows;    // 0 = unknown
    char     craft_name[17];
} fc_state_t;

// FC link timeout (spec 4.5).
#define FC_LINK_TIMEOUT_MS 500

// ---- accessors (fc_state.c) ------------------------------------------------
void fc_state_init(void);

// Copy the whole struct out under the mutex.
void fc_state_get(fc_state_t *dst);

// Run `fn(state, ctx)` under the mutex. `fn` MUST NOT block, log, or touch a
// queue/driver. Used by msp_task to mutate; by others only for tiny reads.
typedef void (*fc_state_mut_fn)(fc_state_t *s, void *ctx);
void fc_state_locked(fc_state_mut_fn fn, void *ctx);

// msp_task-only: atomically replace the shared struct with its private scratch
// copy. The single locked memcpy keeps critical sections copy-only (spec 3.4).
void fc_state_publish(const fc_state_t *scratch);

// Disciplined FC UTC clock (spec 3.4). Returns false if rtc not valid or the
// last RTC sample is older than max_age_ms.
bool fc_now_utc_ms(int64_t *out_ms, uint32_t max_age_ms);
