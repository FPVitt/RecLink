// rid_table.c -- ODID decode into the 8-entry aircraft table (spec 8.8.4 / 8.8.5).
//
// Pure: no RTOS, no logging (received data must never reach the log ring -- spec 8.8.4 privacy),
// no allocation. Only Basic ID and Location messages are decoded, each with opendroneid-core-c's
// per-message decoder into a small stack-local struct (never ODID_UAS_Data / decodeOpenDroneID,
// VERIFY.md RID-CORE-DECODE). Operator ID, Self-ID, System and Auth are discarded unparsed.
#include "rid_core.h"
#include "opendroneid.h"

#include <math.h>
#include <string.h>

_Static_assert(sizeof(ODID_BasicID_encoded) == RID_MSG_SIZE, "core-c message size");
_Static_assert(sizeof(ODID_Location_encoded) == RID_MSG_SIZE, "core-c message size");
_Static_assert(ODID_ID_SIZE == RID_ID_SIZE, "core-c ID size");

void rid_table_clear(rid_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

static int find_by_addr(rid_table_t *t, uint8_t tr, const uint8_t addr[6])
{
    for (int i = 0; i < RID_MAX_AC; i++) {
        const rid_ac_t *a = &t->ac[i];
        if (a->used && (a->src_mask & (1u << tr)) && memcmp(a->addr[tr], addr, 6) == 0) { return i; }
    }
    return -1;
}

static bool entry_stale(const rid_ac_t *a, uint32_t now, uint32_t stale_ms)
{
    return !a->pos_valid || (now - a->last_loc_ms) > stale_ms;
}

// Find a slot for a new transmitter. Eviction when full: unknown-position or stale entries first
// (the oldest of them), then the farthest -- and only if the newcomer is nearer than it; a
// newcomer with no known distance, or farther than every tracked entry, is dropped.
static int alloc_entry(rid_table_t *t, bool new_has_dist, uint32_t new_dist, uint32_t now)
{
    for (int i = 0; i < RID_MAX_AC; i++) { if (!t->ac[i].used) { return i; } }
    // "stale" for eviction = no position, or no Location for 10 s (stale_s is per-config;
    // the table does not see it here, so use the longest valid stale_s floor of the LOW duty).
    int victim = -1; uint32_t oldest = 0;
    for (int i = 0; i < RID_MAX_AC; i++) {
        const rid_ac_t *a = &t->ac[i];
        if (entry_stale(a, now, 10000u)) {
            uint32_t age = now - a->last_any_ms;
            if (victim < 0 || age > oldest) { victim = i; oldest = age; }
        }
    }
    if (victim >= 0) { return victim; }
    if (!new_has_dist) { return -1; }
    int far = -1; uint32_t far_d = 0;
    for (int i = 0; i < RID_MAX_AC; i++) {
        const rid_ac_t *a = &t->ac[i];
        uint32_t d = a->dist_valid ? a->dist_m : UINT32_MAX;
        if (far < 0 || d > far_d) { far = i; far_d = d; }
    }
    if (far >= 0 && new_dist < far_d) { return far; }
    return -1;
}

static int get_entry(rid_table_t *t, uint8_t tr, const uint8_t addr[6], bool has_dist,
                     uint32_t dist, uint32_t now)
{
    int i = find_by_addr(t, tr, addr);
    if (i >= 0) { return i; }
    i = alloc_entry(t, has_dist, dist, now);
    if (i < 0) { t->evict_drops++; return -1; }
    rid_ac_t *a = &t->ac[i];
    memset(a, 0, sizeof(*a));
    a->used = true;
    a->src_mask = (uint8_t)(1u << tr);
    memcpy(a->addr[tr], addr, 6);
    a->first_seen_ms = now;
    a->last_any_ms = now;
    return i;
}

// Higher rank wins when an aircraft sends two Basic IDs (up to 2 per spec): serial > CAA > rest.
static int id_rank(uint8_t type)
{
    switch (type) {
    case ODID_IDTYPE_SERIAL_NUMBER:        return 3;
    case ODID_IDTYPE_CAA_REGISTRATION_ID:  return 2;
    case ODID_IDTYPE_NONE:                 return 0;
    default:                               return 1;
    }
}

static bool printable_id(const char *s)
{
    if (!s[0]) { return false; }
    for (int i = 0; i < RID_ID_SIZE && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) { return false; }
    }
    return true;
}

// Fold entry `o` into `e` (same UAS ID heard on the other transport): one aircraft, counted once.
static void merge(rid_table_t *t, int ei, int oi)
{
    rid_ac_t *e = &t->ac[ei], *o = &t->ac[oi];
    for (int tr = 0; tr < RID_TR_N; tr++) {
        if ((o->src_mask & (1u << tr)) && !(e->src_mask & (1u << tr))) {
            memcpy(e->addr[tr], o->addr[tr], 6);
            e->src_mask |= (uint8_t)(1u << tr);
        }
    }
    if (o->pos_valid && (!e->pos_valid || (int32_t)(o->last_loc_ms - e->last_loc_ms) > 0)) {
        e->pos_valid = true; e->lat_1e7 = o->lat_1e7; e->lon_1e7 = o->lon_1e7;
        e->last_loc_ms = o->last_loc_ms; e->status = o->status;
        e->height_valid = o->height_valid; e->height_m = o->height_m; e->height_ref = o->height_ref;
    }
    if ((int32_t)(o->first_seen_ms - e->first_seen_ms) < 0) { e->first_seen_ms = o->first_seen_ms; }
    if ((int32_t)(o->last_any_ms - e->last_any_ms) > 0)     { e->last_any_ms = o->last_any_ms; }
    memset(o, 0, sizeof(*o));
}

static int ingest_basic_id(rid_table_t *t, uint8_t tr, const uint8_t addr[6],
                           const uint8_t msg[RID_MSG_SIZE], uint32_t now)
{
    ODID_BasicID_encoded enc;
    memcpy(&enc, msg, sizeof(enc));                       // aligned local copy
    ODID_BasicID_data d;
    memset(&d, 0, sizeof(d));
    if (decodeBasicIDMessage(&d, &enc) != ODID_SUCCESS) { return -1; }
    // Non-serial/CAA types memcpy 21 bytes and are not NUL-terminated (VERIFY.md RID-CORE-DECODE).
    d.UASID[RID_ID_SIZE] = '\0';
    if (d.IDType == ODID_IDTYPE_NONE) { return -1; }

    int i = get_entry(t, tr, addr, false, 0, now);
    if (i < 0) { return -1; }
    rid_ac_t *a = &t->ac[i];
    a->last_any_ms = now;                                // UA type: decoded, not kept
    if (!a->id_valid || id_rank((uint8_t)d.IDType) >= id_rank(a->id_type)) {
        memcpy(a->uas_id, d.UASID, RID_ID_SIZE);
        a->uas_id[RID_ID_SIZE] = '\0';
        a->id_type = (uint8_t)d.IDType;
        a->id_valid = true;
        a->id_printable = (d.IDType == ODID_IDTYPE_SERIAL_NUMBER ||
                           d.IDType == ODID_IDTYPE_CAA_REGISTRATION_ID) && printable_id(a->uas_id);
    }
    // Merge-on-ID: another entry (the other transport) with the same ID is the same aircraft.
    for (int k = 0; k < RID_MAX_AC; k++) {
        if (k == i || !t->ac[k].used || !t->ac[k].id_valid) { continue; }
        if (memcmp(t->ac[k].uas_id, a->uas_id, RID_ID_SIZE) == 0) { merge(t, i, k); }
    }
    return i;
}

static int ingest_location(rid_table_t *t, uint8_t tr, const uint8_t addr[6],
                           const uint8_t msg[RID_MSG_SIZE], const rid_own_t *own, uint32_t now)
{
    ODID_Location_encoded enc;
    memcpy(&enc, msg, sizeof(enc));
    ODID_Location_data d;
    memset(&d, 0, sizeof(d));
    if (decodeLocationMessage(&d, &enc) != ODID_SUCCESS) { return -1; }

    // The decoder does no range check (int32/1e7 can reach +-214.7 deg): reject here. Rounding
    // the double back to 1e-7 deg is exact for every int32 input.
    double la = d.Latitude * 1e7, lo = d.Longitude * 1e7;
    bool unknown = false, sane = true;
    int32_t lat = 0, lon = 0;
    if (!(la >= -2147483648.0 && la <= 2147483647.0 && lo >= -2147483648.0 && lo <= 2147483647.0)) {
        sane = false;
    } else {
        lat = (int32_t)llround(la); lon = (int32_t)llround(lo);
        if (lat == 0 && lon == 0) { unknown = true; }
        else if (!rid_latlon_ok(lat, lon)) { sane = false; }
    }
    if (!sane) { t->range_rejects++; return -1; }

    uint32_t nd = 0; bool nd_ok = false;
    if (!unknown && own && own->ok) { nd_ok = rid_geo(own->lat_1e7, own->lon_1e7, lat, lon, &nd, NULL); }
    int i = get_entry(t, tr, addr, nd_ok, nd, now);
    if (i < 0) { return -1; }
    rid_ac_t *a = &t->ac[i];
    a->last_any_ms = now;

    // Only position, status and HEIGHT (+ its reference, for above/below) survive the decode;
    // baro / geodetic altitude, speeds, direction, accuracies and timestamp are dropped here.
    if (unknown) {                                     // 0/0 = position unknown -> unconfirmed
        a->pos_valid = false;
        a->height_valid = false;
        a->status = (uint8_t)d.Status;
        return i;
    }
    // Plausibility: a jump implying more than 255 m/s (+ GPS slack) from this aircraft's previous
    // good sample is rejected and the previous position kept (spec 8.8.5).
    if (a->pos_valid) {
        uint32_t dt = now - a->last_loc_ms;
        uint64_t allow = (uint64_t)RID_MAX_SPEED_MPS * dt / 1000u + RID_JUMP_SLACK_M;
        uint32_t jd = 0;
        bool near = rid_geo(a->lat_1e7, a->lon_1e7, lat, lon, &jd, NULL);
        if ((near && jd > allow) || (!near && allow < RID_MAX_TRACK_M)) {
            t->jump_rejects++;
            return i;
        }
    }
    a->pos_valid = true;
    a->lat_1e7 = lat; a->lon_1e7 = lon;
    a->last_loc_ms = now;
    a->status = (uint8_t)d.Status;
    // Height: INV_ALT (-1000) = unknown (VERIFY.md RID-CORE-INV); the decoder does no range check.
    if (d.Height != (float)INV_ALT && d.Height > -1000.0f && d.Height <= MAX_ALT) {
        a->height_valid = true;
        a->height_m = (int16_t)lroundf(d.Height);
        a->height_ref = (uint8_t)(d.HeightType ? 1 : 0);
    } else {
        a->height_valid = false;
    }
    return i;
}

int rid_table_ingest_msg(rid_table_t *t, uint8_t tr, const uint8_t addr[6],
                         const uint8_t msg[RID_MSG_SIZE], const rid_own_t *own, uint32_t now)
{
    if (!t || !addr || !msg || tr >= RID_TR_N) { return -1; }
    switch (msg[0] >> 4) {                                  // decodeMessageType = byte0 >> 4
    case ODID_MESSAGETYPE_BASIC_ID: return ingest_basic_id(t, tr, addr, msg, now);
    case ODID_MESSAGETYPE_LOCATION: return ingest_location(t, tr, addr, msg, own, now);
    default:                        return -1;              // discarded unparsed (privacy)
    }
}

int rid_table_ingest_pack(rid_table_t *t, const uint8_t addr[6],
                          const uint8_t *pack, size_t len, const rid_own_t *own, uint32_t now)
{
    const uint8_t *m[RID_PACK_MAX_MSGS];
    int n = rid_pack_split(pack, len, m);
    if (n < 0) { return -1; }
    int done = 0;
    // Basic ID first: once the ID is known the Location in the same pack lands on the merged entry.
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < n; k++) {
            bool is_id = (m[k][0] >> 4) == ODID_MESSAGETYPE_BASIC_ID;
            if (is_id != (pass == 0)) { continue; }
            if (rid_table_ingest_msg(t, RID_TR_WIFI, addr, m[k], own, now) >= 0) { done++; }
        }
    }
    return done;
}

bool rid_id_ignored(const rid_params_t *p, const char *id)
{
    if (!p || !p->ignore || !id || !id[0]) { return false; }
    for (int i = 0; i < p->n_ignore; i++) {
        if (p->ignore[i][0] && strncmp(p->ignore[i], id, RID_ID_SIZE) == 0) { return true; }
    }
    return false;
}

void rid_table_update(rid_table_t *t, const rid_params_t *p, const rid_own_t *own,
                      uint32_t now, rid_pick_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t stale_ms = (uint32_t)p->stale_s * 1000u;
    int best = -1;
    for (int i = 0; i < RID_MAX_AC; i++) {
        rid_ac_t *a = &t->ac[i];
        if (!a->used) { continue; }
        if (now - a->last_any_ms > RID_ENTRY_EXPIRE_MS) { memset(a, 0, sizeof(*a)); continue; }
        a->dist_valid = false; a->eligible = false; a->level = RID_LVL_NONE;
        a->hyst_level = RID_LVL_NONE; a->alert_ok = false;
        a->ignored = a->id_valid && a->id_printable && rid_id_ignored(p, a->uas_id);
        if (a->pos_valid && own && own->ok) {
            a->dist_valid = rid_geo(own->lat_1e7, own->lon_1e7, a->lat_1e7, a->lon_1e7,
                                    &a->dist_m, &a->brg_deg);
        }
        a->eligible = !a->ignored && a->pos_valid && a->dist_valid &&
                      (now - a->last_loc_ms) <= stale_ms;
        // Vertical relation: other height vs own height above the arming point (both known).
        bool vh = a->dist_valid && a->height_valid && own && own->alt_ok;
        int32_t dv = 0;
        if (vh) {
            int32_t own_m = (own->alt_cm >= 0 ? own->alt_cm + 50 : own->alt_cm - 50) / 100;
            dv = (int32_t)a->height_m - own_m;
            if (dv > 32767) { dv = 32767; } else if (dv < -32767) { dv = -32767; }
        }
        a->vert = rid_vert_step(a->vert, vh, dv);
        a->dv_m = (int16_t)dv;
        if (!a->eligible) { continue; }

        // Caps: an un-identified entry stays at NOTICE until its ID is heard or the 15 s grace
        // passes (your own module must not flash an ALERT before the ignore list can match it);
        // an aircraft reporting "ground" stays at NOTICE unless rid.warn.ground.
        bool capped = (!a->id_valid && now - a->first_seen_ms < RID_ID_GRACE_MS) ||
                      (a->status == RID_STATUS_GROUND && !p->ground);
        uint8_t lvl = rid_level_for(a->dist_m, p, 100);
        if (capped && lvl > RID_LVL_NOTICE) { lvl = RID_LVL_NOTICE; }
        a->level = lvl;
        {
            uint8_t hl0 = rid_level_for(a->dist_m, p, 115);
            if (capped && hl0 > RID_LVL_NOTICE) { hl0 = RID_LVL_NOTICE; }
            a->hyst_level = hl0;
            a->alert_ok = !capped;
        }

        if (!out->near_have || a->dist_m < out->near_dist_m) {
            out->near_have = true; out->near_dist_m = a->dist_m;
        }
        if (a->dist_m <= p->notice_m && out->count < 255) { out->count++; }
        // Most severe first, nearest on a tie. An eligible aircraft beyond notice_m is still a
        // candidate (level NONE) so NOTICE -> NONE goes through the hysteresis hold too.
        if (best < 0 || lvl > t->ac[best].level ||
            (lvl == t->ac[best].level && a->dist_m < t->ac[best].dist_m)) { best = i; }
    }
    if (best >= 0) {
        const rid_ac_t *a = &t->ac[best];
        bool capped = (!a->id_valid && now - a->first_seen_ms < RID_ID_GRACE_MS) ||
                      (a->status == RID_STATUS_GROUND && !p->ground);
        uint8_t hl = rid_level_for(a->dist_m, p, 115);
        if (capped && hl > RID_LVL_NOTICE) { hl = RID_LVL_NOTICE; }
        out->have = true;
        out->level = a->level;
        out->hyst_level = hl;
        out->alert_ok = !capped;
        out->dist_m = a->dist_m;
        out->brg_deg = a->brg_deg;
        out->vert = a->vert;
        out->dv_m = a->dv_m;
    }
}
