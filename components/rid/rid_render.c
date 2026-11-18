// rid_render.c -- the one shared %rp formatter (spec 8.8.7), used by the token path AND the
// line-replacement path. Pure; host-tested for every worst case (<= 16 characters).
//
//   !RIDx <dist> <brg><v>
//   "!" : always first; it is the only part that blinks (a space while it is blinked off)
//   x   : the aircraft's id 1..9 (one aircraft per message; stable while it is tracked)
//   dist: metric NNNM then N.NKM; imperial NNNFT below 1000 ft, then N.NMI
//   brg : arrow glyph (1 char, patched after the digital filter) / compass (<= 2) / degrees (3)
//   v   : '+' other aircraft above you, '-' below you, nothing when level / unknown (always text)
#include "rid_core.h"

#include <stdio.h>
#include <string.h>

static size_t put(char *out, size_t cap, size_t len, const char *s)
{
    while (*s && len + 1 < cap) { out[len++] = *s++; }
    out[len] = '\0';
    return len;
}

static void fmt_dist(char *b, size_t n, uint32_t m, bool imperial)
{
    if (!imperial) {
        if (m < 1000u) { snprintf(b, n, "%luM", (unsigned long)m); return; }
        uint32_t hm = (m + 50u) / 100u;                            // 0.1 km
        if (hm >= 100u) { snprintf(b, n, "%luKM", (unsigned long)(hm / 10u)); return; }
        snprintf(b, n, "%lu.%luKM", (unsigned long)(hm / 10u), (unsigned long)(hm % 10u));
        return;
    }
    // 1 km is 3280 ft, so feet would overflow the line: feet below 1000 ft, then miles.
    uint32_t ft = (uint32_t)(((uint64_t)m * 328084u + 50000u) / 100000u);
    if (ft < 1000u) { snprintf(b, n, "%luFT", (unsigned long)ft); return; }
    uint32_t tmi = (uint32_t)(((uint64_t)m * 10000u + 804672u) / 1609344u);   // 0.1 mi
    if (tmi >= 100u) { snprintf(b, n, "%luMI", (unsigned long)(tmi / 10u)); return; }
    snprintf(b, n, "%lu.%luMI", (unsigned long)(tmi / 10u), (unsigned long)(tmi % 10u));
}

static size_t glyph_at(char *out, size_t cap, size_t len, rid_patch_t *pt, int k, uint8_t g)
{
    if (len + 1 >= cap) { return len; }
    pt->idx[k] = (int8_t)len;
    pt->glyph[k] = g;
    out[len++] = RID_ARROW_PLACEHOLDER;
    out[len] = '\0';
    return len;
}

static int render_once(char *out, size_t cap, const rid_render_in_t *in, rid_patch_t *pt)
{
    pt->idx[0] = pt->idx[1] = -1;
    pt->glyph[0] = pt->glyph[1] = 0;
    char tmp[16];
    size_t len = 0;
    out[0] = '\0';
    len = put(out, cap, len, in->bang_on ? "!" : " ");     // the only thing that blinks
    len = put(out, cap, len, "RID");
    if (in->index >= 1) {
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)(in->index > 9 ? 9 : in->index));
        len = put(out, cap, len, tmp);
    }
    len = put(out, cap, len, " ");
    fmt_dist(tmp, sizeof(tmp), in->dist_m, in->imperial);
    len = put(out, cap, len, tmp);

    // Bearing. A wrong arrow is worse than none (spec 9.4): the arrow only with a valid heading;
    // `auto` falls back to compass text, `glyph` shows nothing.
    uint8_t style = in->bearing;
    if (style == RID_BRG_AUTO) { style = in->hdg_valid ? RID_BRG_GLYPH : RID_BRG_COMPASS; }
    bool brg_shown = false;
    switch (style) {
    case RID_BRG_GLYPH:
        if (in->hdg_valid) {
            len = put(out, cap, len, " ");
            len = glyph_at(out, cap, len, pt, 0, rid_arrow_glyph(in->brg_deg, in->hdg_deg));
            brg_shown = true;
        }
        break;
    case RID_BRG_COMPASS:
        len = put(out, cap, len, " ");
        len = put(out, cap, len, rid_compass8(in->brg_deg));
        brg_shown = true;
        break;
    case RID_BRG_DEGREES:
        snprintf(tmp, sizeof(tmp), " %03u", (unsigned)(in->brg_deg % 360u));
        len = put(out, cap, len, tmp);
        brg_shown = true;
        break;
    default:
        break;
    }
    // Vertical indicator (owner decision 2026-11-18): '+' above you / '-' below you, ALWAYS text
    // (never a glyph), right after the bearing; nothing for LEVEL / unknown.
    if (in->vert == RID_VERT_ABOVE || in->vert == RID_VERT_BELOW) {
        if (!brg_shown) { len = put(out, cap, len, " "); }
        len = put(out, cap, len, in->vert == RID_VERT_ABOVE ? "+" : "-");
    }
    return (int)len;
}

int rid_render(char *out, size_t cap, const rid_render_in_t *in, rid_patch_t *pt)
{
    rid_patch_t dummy;
    if (!pt) { pt = &dummy; }
    pt->idx[0] = pt->idx[1] = -1;
    pt->glyph[0] = pt->glyph[1] = 0;
    if (!out || cap == 0) { return 0; }
    out[0] = '\0';
    if (!in || cap < 2) { return 0; }

    if (in->kind == RID_R_FAIL) {
        size_t len = put(out, cap, 0, "RID RX FAIL");               // 11 plain-ASCII characters
        return (int)len;
    }
    if (in->kind != RID_R_PROX || in->level < RID_LVL_NOTICE || in->level > RID_LVL_ALERT) {
        return 0;
    }
    // Worst case is exactly 16 ("!RID1 1.2KM 045+"); the clamp below is purely defensive (spec 9.1).
    char tmp[24];
    int len = render_once(tmp, sizeof(tmp), in, pt);
    if (len > 16) {
        tmp[16] = '\0'; len = 16;
        for (int k = 0; k < 2; k++) { if (pt->idx[k] >= 16) { pt->idx[k] = -1; } }
    }
    size_t n = put(out, cap, 0, tmp);
    for (int k = 0; k < 2; k++) { if (pt->idx[k] >= (int)n) { pt->idx[k] = -1; } }
    return (int)n;
}

// ---- the message sequence + the blinking "!" (owner decision 2026-11-18) -------------------------
void rid_seq_select(const rid_msg_t *msgs, uint8_t n, uint32_t now, rid_seq_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!msgs || n == 0) { return; }
    if (n > RID_MAX_AC) { n = RID_MAX_AC; }
    uint8_t na = 0;                                   // msgs are sorted level-descending: ALERTs first
    while (na < n && msgs[na].level >= RID_LVL_ALERT) { na++; }
    if (na > 0) {                                     // ALERT priority: only alerts, one after the other
        out->replace = true;
        out->alert = true;
        out->idx = (uint8_t)((now / RID_MSG_MS) % na);
        out->level = msgs[out->idx].level;
        return;
    }
    uint32_t cycle = RID_NORMAL_MS + (uint32_t)n * RID_MSG_MS;   // normal line, then each message
    uint32_t t = now % cycle;
    if (t < RID_NORMAL_MS) { return; }                // normal OSD line phase
    out->replace = true;
    out->idx = (uint8_t)((t - RID_NORMAL_MS) / RID_MSG_MS);
    out->level = msgs[out->idx].level;
}

bool rid_bang_on(uint8_t level, uint32_t now, uint8_t refresh_hz, bool blink_enabled)
{
    if (!blink_enabled) { return true; }
    if (level >= RID_LVL_ALERT) { return refresh_hz < 4 ? true : (((now / 250u) & 1u) == 0u); }   // fast, 2 Hz
    if (level == RID_LVL_WARN)  { return refresh_hz < 2 ? true : (((now / 500u) & 1u) == 0u); }   // slow, 1 Hz
    return true;                                                                                 // NOTICE: steady
}
