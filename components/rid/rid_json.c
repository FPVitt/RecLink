// rid_json.c -- the per-aircraft JSON object for the bench portal (spec 8.8.10), one pure function
// shared by GET /api/rid and GET /api/rid/test, host-tested. Data minimisation (owner decision
// 2026-11-18): it emits ONLY the identity, the transport, the age and -- when computed -- range,
// bearing and level. No position, no RSSI, no message contents, ever.
#include "rid_core.h"

#include <stdio.h>
#include <string.h>

// JSON string body for a printable UAS ID (escapes " and \).
static void json_esc(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < cap; i++) {
        char ch = in[i];
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; }
        out[o++] = ch;
    }
    out[o] = '\0';
}

// The UAS ID (escaped), HEX:... for binary ID types, or BLE:/WIFI: + the source address as a
// pseudo-ID until a Basic ID has been heard.
static void ac_id(const rid_ac_t *a, char *id, size_t cap)
{
    if (a->id_valid && a->id_printable) {
        json_esc(id, cap, a->uas_id);
    } else if (a->id_valid) {
        int k = snprintf(id, cap, "HEX:");
        int last = RID_ID_SIZE - 1;
        while (last > 0 && a->uas_id[last] == 0) { last--; }
        for (int q = 0; q <= last && k + 3 < (int)cap; q++) {
            k += snprintf(id + k, cap - (size_t)k, "%02X", (uint8_t)a->uas_id[q]);
        }
    } else {
        int tr = (a->src_mask & 1u) ? RID_TR_BLE : RID_TR_WIFI;
        const uint8_t *m = a->addr[tr];
        snprintf(id, cap, "%s:%02X:%02X:%02X:%02X:%02X:%02X", tr == RID_TR_BLE ? "BLE" : "WIFI",
                 m[5], m[4], m[3], m[2], m[1], m[0]);
    }
}

int rid_ac_json(char *out, size_t cap, const rid_ac_t *a, uint32_t now, bool table_fields,
                uint8_t num)
{
    static const char *const k_lvl[] = { "none", "notice", "warn", "alert" };
    if (!out || cap < RID_AC_JSON_MAX || !a) { if (out && cap) { out[0] = '\0'; } return 0; }
    char id[48];
    ac_id(a, id, sizeof(id));
    const char *src = a->src_mask == 3 ? "both" : (a->src_mask & 1u) ? "ble" : "wifi";
    int n = snprintf(out, cap, "{\"id\":\"%s\",\"src\":\"%s\",\"age_ms\":%lu", id, src,
                     (unsigned long)(now - a->last_any_ms));
    if (num >= 1 && num <= 9) {                                     // the "RIDx" number on the OSD
        n += snprintf(out + n, cap - (size_t)n, ",\"n\":%u", (unsigned)num);
    }
    if (table_fields) {
        n += snprintf(out + n, cap - (size_t)n, ",\"ignored\":%s", a->ignored ? "true" : "false");
        if (a->eligible) {
            n += snprintf(out + n, cap - (size_t)n, ",\"level\":\"%s\"", k_lvl[a->level <= 3 ? a->level : 0]);
        }
    }
    if (a->dist_valid) {
        n += snprintf(out + n, cap - (size_t)n, ",\"dist_m\":%lu,\"brg\":%u",
                      (unsigned long)a->dist_m, (unsigned)a->brg_deg);
    }
    if (a->dist_valid && a->vert != RID_VERT_UNKNOWN) {             // raw heights never exposed
        static const char *const k_v[] = { "", "above", "below", "level" };
        n += snprintf(out + n, cap - (size_t)n, ",\"vert\":\"%s\",\"dv_m\":%d",
                      k_v[a->vert <= 3 ? a->vert : 0], (int)a->dv_m);
    }
    n += snprintf(out + n, cap - (size_t)n, "}");
    return n;
}
