// hdg_align.c -- the hdg_aligned latch (spec 8.8.5); see hdg_align.h.
#include "hdg_align.h"

void hdg_align_reset(hdg_align_t *h)
{
    h->aligned = false;
    h->fast_since = 0;
    h->disarm_since = 0;
}

bool hdg_align_step(hdg_align_t *h, const hdg_align_in_t *in, uint32_t now)
{
    if (!in->link_up) {                              // link loss resets the latch
        hdg_align_reset(h);
        return false;
    }
    // Disarm hold: a decoded disarm held >= 3 s, and not "maybe airborne" -> release.
    if (in->armed_valid && !in->armed) {
        if (!h->disarm_since) { h->disarm_since = now ? now : 1; }
        if (now - h->disarm_since >= HDG_DISARM_HOLD_MS && !in->airborne_hint) {
            h->aligned = false;
            h->fast_since = 0;
            return false;
        }
    } else {
        h->disarm_since = 0;
    }
    bool fresh = in->gps_rx_ms != 0 && (now - in->gps_rx_ms) <= HDG_GPS_MAX_AGE_MS;
    bool fast = fresh && in->fix >= 3 && in->sats >= HDG_MIN_SATS && in->spd_cm_s >= HDG_MIN_SPEED_CM_S;
    if (fast) {
        if (!h->fast_since) { h->fast_since = now ? now : 1; }
        if (now - h->fast_since >= HDG_MIN_HOLD_MS) { h->aligned = true; }
    } else {
        h->fast_since = 0;
    }
    return h->aligned;
}
