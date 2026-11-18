// hdg_align.h -- is Betaflight's yaw trustworthy for the Remote ID arrow? (spec 8.8.5)
//
// Without a compass (or with one Betaflight is not using -- MSP_STATUS bit 2 means "detected",
// not "used") Betaflight's yaw is relative to the power-on orientation until GPS course-over-ground
// has corrected it, which it does only slowly and only in forward flight with > 4 satellites.
// The FC cannot tell us when that has happened, so the arrow waits for evidence: ground speed
// >= 5 m/s for >= 5 s with >= 5 satellites and a 3D fix since the FC link came up. The latch is
// required with or without a compass, and is released on FC link loss or when the disarm hold
// ends (a decoded disarm held >= 3 s while the aircraft is not "maybe airborne").
// Pure; single writer msp_task; host-tested (test_hdg.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HDG_MIN_SPEED_CM_S  500u     // 5 m/s
#define HDG_MIN_HOLD_MS     5000u
#define HDG_MIN_SATS        5u
#define HDG_GPS_MAX_AGE_MS  1500u    // RAW_GPS is polled at 2 Hz: a missed sample breaks the run
#define HDG_DISARM_HOLD_MS  3000u    // = RID_WIFI_STOP_HOLD_MS

typedef struct {
    bool     aligned;
    uint32_t fast_since;             // 0 = condition not running
    uint32_t disarm_since;           // 0 = not (decoded) disarmed
} hdg_align_t;

typedef struct {
    bool     link_up;
    bool     armed_valid, armed;
    bool     airborne_hint;          // flight_may_be_airborne()
    uint8_t  fix, sats;
    uint16_t spd_cm_s;
    uint32_t gps_rx_ms;              // 0 = never
} hdg_align_in_t;

void hdg_align_reset(hdg_align_t *h);
// Returns the latched hdg_aligned.
bool hdg_align_step(hdg_align_t *h, const hdg_align_in_t *in, uint32_t now);
