#include "osd.h"
#include <stdio.h>
#include <string.h>

// ctrl-fed warning latches (spec 9.5 items 3 / F6 fail).
static volatile bool s_rec_fail;
static volatile bool s_clock_fail;
void osd_set_rec_fail(bool on)   { s_rec_fail = on; }
void osd_set_clock_fail(bool on) { s_clock_fail = on; }

static void mmss(char *b, size_t n, uint32_t secs)
{
    snprintf(b, n, "%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

static void remain_str(char *b, size_t n, uint32_t secs)
{
    if (secs >= 3600) { snprintf(b, n, "%luh", (unsigned long)(secs / 3600)); }
    else              { snprintf(b, n, "%lum", (unsigned long)(secs / 60)); }
}

const char *osd_active_warning(const fc_state_t *fc, const cam_state_t *cam, const cfg_t *cfg)
{
    (void)fc;
    if (cam->link != CAM_LINK_READY) { return "CAM LOST"; }
    if (cam->sd_missing)             { return "NO SD"; }
    if (cam->sd_full)                { return "SD FULL"; }
    if (s_rec_fail)                  { return "REC FAIL"; }
    if (cam->battery_pct.valid && cam->battery_pct.v <= cfg->osd.warn.batt_pct) { return "LOW BATT"; }
    if (cam->rec_remaining_s.valid &&
        cam->rec_remaining_s.v <= (uint32_t)cfg->osd.warn.sd_low_min * 60)      { return "SD LOW"; }
    if (cam->hot)                    { return "HOT"; }
    if (s_clock_fail)                { return "CLK FAIL"; }
    return "";
}

// Append s to out (bounded). Returns new length.
static size_t app(char *out, size_t cap, size_t len, const char *s)
{
    while (*s && len + 1 < cap) { out[len++] = *s++; }
    out[len] = '\0';
    return len;
}

static bool field_on(const cfg_t *cfg, int tok) { return (cfg->osd.fields_mask >> tok) & 1u; }

int osd_format_line(char *out, size_t out_cap, const char *tmpl,
                    const fc_state_t *fc, const cam_state_t *cam,
                    const cfg_t *cfg, bool blink_phase)
{
    size_t len = 0;
    out[0] = '\0';
    char tmp[24];

    for (const char *p = tmpl; *p && len + 1 < out_cap; p++) {
        if (*p != '%') { if (len + 1 < out_cap) { out[len++] = *p; out[len] = '\0'; } continue; }
        p++;
        if (!*p) { break; }
        char a = *p, b = *(p + 1);
        #define TOK2(x, y) (a == (x) && b == (y))

        if (TOK2('s','t')) {                       // recording state
            p++;
            if (!field_on(cfg, OSD_TOK_ST)) { continue; }
            const char *s = "----";
            if (cam->link != CAM_LINK_READY) { s = ""; }        // %wr carries CAM LOST
            else if (cam->pre_record == TRIBOOL_TRUE) { s = "PRE"; }
            else if (cam->recording == TRIBOOL_TRUE)  { s = "REC"; }
            else if (cam->recording == TRIBOOL_FALSE) { s = "IDLE"; }
            else { s = ""; }                                     // unknown -> blank
            len = app(out, out_cap, len, s);
        } else if (TOK2('e','t')) {                // elapsed M:SS
            p++;
            if (!field_on(cfg, OSD_TOK_ET) || !cam->rec_elapsed_s.valid) { continue; }
            mmss(tmp, sizeof(tmp), cam->rec_elapsed_s.v);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('r','t')) {                // remaining
            p++;
            if (!field_on(cfg, OSD_TOK_RT) || !cam->rec_remaining_s.valid) { continue; }
            remain_str(tmp, sizeof(tmp), cam->rec_remaining_s.v);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('b','t')) {                // battery %
            p++;
            if (!field_on(cfg, OSD_TOK_BT) || !cam->battery_pct.valid) { continue; }
            snprintf(tmp, sizeof(tmp), "%u%%", cam->battery_pct.v);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('b','b')) {                // battery bar glyphs
            p++;
            if (!field_on(cfg, OSD_TOK_BB) || !cam->battery_pct.valid) { continue; }
            int bars = (cam->battery_pct.v + 12) / 25; if (bars > 4) bars = 4;
            for (int i = 0; i < 4; i++) { tmp[i] = (char)(i < bars ? 0xFE : 0xFA); }
            tmp[4] = '\0';
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('r','s')) {                // resolution
            p++;
            if (!field_on(cfg, OSD_TOK_RS) || !cam->resolution.valid) { continue; }
            len = app(out, out_cap, len, cam->resolution.v);
        } else if (TOK2('f','p')) {                // framerate
            p++;
            if (!field_on(cfg, OSD_TOK_FP) || !cam->framerate.valid) { continue; }
            len = app(out, out_cap, len, cam->framerate.v);
        } else if (TOK2('l','n')) {                // lens
            p++;
            if (!field_on(cfg, OSD_TOK_LN) || !cam->lens.valid) { continue; }
            len = app(out, out_cap, len, cam->lens.v);
        } else if (TOK2('s','d')) {                // SD remaining
            p++;
            if (!field_on(cfg, OSD_TOK_SD)) { continue; }
            if (cam->sd_missing) { len = app(out, out_cap, len, "NO SD"); }
            else if (cam->sd_full) { len = app(out, out_cap, len, "FULL"); }
            else if (cam->rec_remaining_s.valid) {
                remain_str(tmp, sizeof(tmp), cam->rec_remaining_s.v);
                len = app(out, out_cap, len, tmp);
            } else if (cam->sd_remaining_kb.valid) {
                snprintf(tmp, sizeof(tmp), "%luG", (unsigned long)(cam->sd_remaining_kb.v / (1024 * 1024)));
                len = app(out, out_cap, len, tmp);
            }
        } else if (TOK2('s','g')) {                // SD space remaining, GB (always)
            p++;
            if (!field_on(cfg, OSD_TOK_SG) || !cam->sd_remaining_kb.valid) { continue; }
            uint32_t gx10 = (cam->sd_remaining_kb.v / 1024u) * 10u / 1024u;   // KB -> GB x10
            snprintf(tmp, sizeof(tmp), "%lu.%luG",
                     (unsigned long)(gx10 / 10), (unsigned long)(gx10 % 10));
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('m','d')) {                // mode name
            p++;
            if (!field_on(cfg, OSD_TOK_MD) || !cam->mode_name.valid) { continue; }
            len = app(out, out_cap, len, cam->mode_name.v);
        } else if (TOK2('c','n')) {                // craft name (FC)
            p++;
            if (!field_on(cfg, OSD_TOK_CN) || !fc->craft_name[0]) { continue; }
            len = app(out, out_cap, len, fc->craft_name);
        } else if (TOK2('w','r')) {                // active warning
            p++;
            if (!field_on(cfg, OSD_TOK_WR)) { continue; }
            const char *w = osd_active_warning(fc, cam, cfg);
            if (*w && !(cfg->osd.warn.blink && blink_phase)) { len = app(out, out_cap, len, w); }
        } else if (TOK2('a','r')) {                // aspect ratio
            p++;
            if (!field_on(cfg, OSD_TOK_AR) || !cam->aspect.valid) { continue; }
            len = app(out, out_cap, len, cam->aspect.v);
        } else if (TOK2('p','r')) {                // colour profile (STD/LOG/HDR/HLG)
            p++;
            if (!field_on(cfg, OSD_TOK_PR) || !cam->profile.valid) { continue; }
            len = app(out, out_cap, len, cam->profile.v);
        } else if (TOK2('b','d')) {                // bit depth (8/10)
            p++;
            if (!field_on(cfg, OSD_TOK_BD) || !cam->bit_depth.valid) { continue; }
            snprintf(tmp, sizeof(tmp), "%ub", cam->bit_depth.v);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('c','r')) {                // camera link RSSI
            p++;
            if (!field_on(cfg, OSD_TOK_CR) || cam->link != CAM_LINK_READY) { continue; }
            snprintf(tmp, sizeof(tmp), "%d", cam->rssi);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('g','s')) {                // GPS sat count (FC)
            p++;
            if (!field_on(cfg, OSD_TOK_GS) || !fc->gps_present) { continue; }
            snprintf(tmp, sizeof(tmp), "%u", fc->gps.sats);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('g','f')) {                // GPS fix (2D/3D/NO)
            p++;
            if (!field_on(cfg, OSD_TOK_GF) || !fc->gps_present) { continue; }
            len = app(out, out_cap, len, fc->gps.fix >= 3 ? "3D" : fc->gps.fix == 2 ? "2D" : "NO");
        } else if (TOK2('f','b')) {                // FC battery state
            p++;
            if (!field_on(cfg, OSD_TOK_FB)) { continue; }
            static const char *bs[] = { "OK", "LOW", "CRIT", "" };
            len = app(out, out_cap, len, bs[fc->batt_state & 3]);
        } else if (TOK2('s','p')) {                // ground speed (FC GPS), unit per cfg.osd.units
            p++;
            if (!field_on(cfg, OSD_TOK_SP) || !fc->gps_present || fc->gps.fix < 2) { continue; }
            // cfg.osd.units: 0 = metric (km/h), 1 = imperial (mph)
            if (cfg->osd.units) {
                snprintf(tmp, sizeof(tmp), "%luMPH",
                         (unsigned long)((fc->gps.spd_cm_s * 224u + 5000u) / 10000u));
            } else {
                snprintf(tmp, sizeof(tmp), "%luKMH",
                         (unsigned long)((fc->gps.spd_cm_s * 36u + 500u) / 1000u));
            }
            len = app(out, out_cap, len, tmp);
        } else {
            // unknown token: emit literally
            if (len + 2 < out_cap) { out[len++] = '%'; out[len++] = a; out[len] = '\0'; }
        }
        #undef TOK2
    }

    // hard 16-char clamp (spec 9.1)
    if (len > 16) { out[16] = '\0'; len = 16; }
    return (int)len;
}
