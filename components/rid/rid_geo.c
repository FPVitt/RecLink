// rid_geo.c -- geometry, heading arrow, levels + hysteresis, the GPS activation gate (spec 8.8.5).
// Pure functions: unit-tested on the host (test_rid.c).
#include "rid_core.h"

#include <math.h>
#include <stdlib.h>

#define M_PER_1E7_DEG 0.0111195f              // metres per 1e-7 degree of latitude (spec 8.8.5)
#define DEG2RAD       0.017453292519943295f

bool rid_latlon_ok(int32_t lat, int32_t lon)
{
    if (lat == 0 && lon == 0) { return false; }                       // ODID "unknown"
    if (lat > 900000000 || lat < -900000000) { return false; }
    if (lon > 1800000000 || lon < -1800000000) { return false; }
    return true;
}

bool rid_geo(int32_t lat0, int32_t lon0, int32_t lat1, int32_t lon1,
             uint32_t *dist_m, uint16_t *brg_deg)
{
    if (!rid_latlon_ok(lat0, lon0) || !rid_latlon_ok(lat1, lon1)) { return false; }
    // int64: an int32 delta overflows with hostile +-180 deg values or across the antimeridian.
    int64_t dlat = (int64_t)lat1 - (int64_t)lat0;
    int64_t dlon = (int64_t)lon1 - (int64_t)lon0;
    if (dlon >  1800000000LL) { dlon -= 3600000000LL; }               // shortest way round
    if (dlon < -1800000000LL) { dlon += 3600000000LL; }
    // 20 km gate BEFORE any trigonometry: |dy| needs no cosine. 20000 m / 0.0111195 = 1798642.
    const int64_t k_gate = (int64_t)((float)RID_MAX_TRACK_M / M_PER_1E7_DEG) + 1;
    if (dlat > k_gate || dlat < -k_gate) { return false; }
    // dx = dlon x k x cos(lat_own). |dlon| <= 180e7 here, exact enough in float.
    float c = cosf((float)lat0 * 1e-7f * DEG2RAD);
    if (c < 0.0f) { c = 0.0f; }
    float dx = (float)dlon * M_PER_1E7_DEG * c;
    float dy = (float)dlat * M_PER_1E7_DEG;
    if (dx > (float)RID_MAX_TRACK_M || dx < -(float)RID_MAX_TRACK_M) { return false; }
    float d = sqrtf(dx * dx + dy * dy);
    if (!(d <= (float)RID_MAX_TRACK_M)) { return false; }              // also rejects NaN
    if (dist_m) { *dist_m = (uint32_t)(d + 0.5f); }
    if (brg_deg) {
        float b = atan2f(dx, dy) / DEG2RAD;                            // 0 = north, clockwise
        int bi = (int)lroundf(b);
        bi %= 360; if (bi < 0) { bi += 360; }
        *brg_deg = (uint16_t)bi;
    }
    return true;
}

// Betaflight osdGetDirectionSymbolFromHeading() (osd_elements.c:591 @ 2025.12.5): 16 sectors,
// d = (rel*16 + 180) / 360 mod 16, glyph = SYM_ARROW_SOUTH + ((16 - d + 8) mod 16). Relative
// bearing rel = (brg - hdg) mod 360, like the home arrow (direction - attitude.yaw).
uint8_t rid_arrow_glyph(uint16_t brg_deg, int16_t hdg_deg)
{
    int rel = ((int)brg_deg - (int)hdg_deg) % 360;
    if (rel < 0) { rel += 360; }
    int d = ((rel * 16 + 180) / 360) % 16;
    return (uint8_t)(RID_SYM_ARROW_SOUTH + ((16 - d + 8) % 16));
}

const char *rid_compass8(uint16_t brg_deg)
{
    static const char *const k[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return k[(((unsigned)brg_deg % 360u) * 8u + 180u) / 360u % 8u];
}

// ---- vertical relation --------------------------------------------------------------------------
// Enter ABOVE/BELOW only beyond band + hyst; leave only inside band - hyst (same style as the level
// hysteresis), so the indicator does not flicker at the edge. Unknown input -> UNKNOWN (shown as
// nothing, never LEVEL by default).
uint8_t rid_vert_step(uint8_t prev, bool have, int32_t dv)
{
    if (!have) { return RID_VERT_UNKNOWN; }
    const int32_t in = RID_LEVEL_BAND_M + RID_VERT_HYST_M, out = RID_LEVEL_BAND_M - RID_VERT_HYST_M;
    if (prev == RID_VERT_ABOVE) { if (dv >= out) { return RID_VERT_ABOVE; } }
    if (prev == RID_VERT_BELOW) { if (dv <= -out) { return RID_VERT_BELOW; } }
    if (dv > in)  { return RID_VERT_ABOVE; }
    if (dv < -in) { return RID_VERT_BELOW; }
    if (prev == RID_VERT_UNKNOWN) {            // first classification: the plain band
        if (dv > RID_LEVEL_BAND_M)  { return RID_VERT_ABOVE; }
        if (dv < -RID_LEVEL_BAND_M) { return RID_VERT_BELOW; }
    }
    return RID_VERT_LEVEL;
}

// ---- levels -------------------------------------------------------------------------------------
static uint32_t scaled(uint16_t thr, int scale_pct)
{
    if (scale_pct <= 100) { return thr; }
    uint32_t s = (uint32_t)thr * (uint32_t)scale_pct / 100u;
    if (s < (uint32_t)thr + 10u) { s = (uint32_t)thr + 10u; }        // min +10 m (spec 8.8.5)
    return s;
}

uint8_t rid_level_for(uint32_t dist_m, const rid_params_t *p, int scale_pct)
{
    if (dist_m <= scaled(p->alert_m, scale_pct))  { return RID_LVL_ALERT; }
    if (dist_m <= scaled(p->warn_m, scale_pct))   { return RID_LVL_WARN; }
    if (dist_m <= scaled(p->notice_m, scale_pct)) { return RID_LVL_NOTICE; }
    return RID_LVL_NONE;
}

uint8_t rid_level_step(rid_lvl_state_t *s, const rid_pick_t *pk, const rid_params_t *p,
                       uint32_t own_sample_ms, uint32_t now)
{
    // Two consecutive own-position samples inside alert_m before ALERT (spec 8.8.5): counted
    // per new MSP_RAW_GPS sample, so one GPS glitch or one spoofed point cannot raise it.
    if (own_sample_ms != s->last_own_sample) {
        s->last_own_sample = own_sample_ms;
        bool in_alert = pk->have && pk->alert_ok && pk->dist_m <= p->alert_m;
        if (in_alert) { if (s->alert_streak < 255) { s->alert_streak++; } }
        else          { s->alert_streak = 0; }
    }
    if (!pk->have) {                       // nothing eligible (stale / gone): clear at once (9.4)
        s->cur = RID_LVL_NONE;
        s->below_since = 0;
        s->alert_streak = 0;
        return s->cur;
    }
    uint8_t target = pk->level;
    if (target == RID_LVL_ALERT && s->alert_streak < 2) { target = RID_LVL_WARN; }
    if (target >= s->cur) {                // escalation is immediate
        if (target > s->cur || s->below_since) { s->below_since = 0; }
        s->cur = target;
        return s->cur;
    }
    // De-escalation: only once the hysteretic level (thresholds x1.15, min +10 m) has been
    // below the displayed one for RID_DEESC_HOLD_MS.
    if (pk->hyst_level < s->cur) {
        if (!s->below_since) { s->below_since = now ? now : 1; }
        if (now - s->below_since >= RID_DEESC_HOLD_MS) {
            s->cur = target;
            s->below_since = 0;
        }
    } else {
        s->below_since = 0;
    }
    return s->cur;
}

uint8_t rid_entry_level_step(rid_lvl_state_t *s, const rid_ac_t *a, const rid_params_t *p,
                             uint32_t own_sample_ms, uint32_t now)
{
    rid_pick_t pk = { 0 };
    if (a && a->used && a->eligible) {
        pk.have = true;
        pk.level = a->level;
        pk.hyst_level = a->hyst_level;
        pk.alert_ok = a->alert_ok;
        pk.dist_m = a->dist_m;
        pk.brg_deg = a->brg_deg;
    }
    return rid_level_step(s, &pk, p, own_sample_ms, now);
}

// ---- activation gate --------------------------------------------------------------------------
bool rid_gps_ok_eval(bool link_up, bool gps_present, uint8_t fix, uint8_t sats, uint8_t min_sats,
                     uint32_t gps_rx_ms, uint32_t now)
{
    if (!link_up || !gps_present || fix < 3 || sats < min_sats || gps_rx_ms == 0) { return false; }
    return (now - gps_rx_ms) <= RID_GPS_MAX_AGE_MS;
}

bool rid_gate_step(rid_gate_t *g, bool enabled, bool gps_ok, uint32_t now)
{
    if (!enabled) {
        g->active = false; g->ok_since = 0; g->bad_since = 0;
        return false;
    }
    if (gps_ok) {
        g->bad_since = 0;
        if (!g->ok_since) { g->ok_since = now ? now : 1; }
        if (!g->active && now - g->ok_since >= RID_GPS_SETTLE_MS) { g->active = true; }
    } else {
        g->ok_since = 0;
        if (g->active) {
            if (!g->bad_since) { g->bad_since = now ? now : 1; }
            if (now - g->bad_since >= RID_GPS_LOST_GRACE_MS) { g->active = false; g->bad_since = 0; }
        }
    }
    return g->active;
}
