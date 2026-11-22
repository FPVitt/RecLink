#include "osd.h"
#include "rid_core.h"
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

// Digital-safe 4-segment battery bar. Used by %bb, and as %bi's fallback on
// digital OSD -- the DJI-goggle font has no true battery-icon glyph.
static void batt_bar4(char *tmp, int pct, bool digital)
{
    int bars = (pct + 12) / 25; if (bars > 4) { bars = 4; }
    // VERIFY: analog bytes 0xFE/0xFA are NOT defined in BF osd_symbols.h @
    // pin (tag 2025.12.5, commit 7348054f) and are not referenced by
    // osd_elements.c's bar-drawing code -- placeholder, unconfirmed as a
    // real solid/empty-block glyph. See VERIFY.md MSP-OSD-SYM-BATTBAR.
    char full = digital ? '#' : (char)0xFE;
    char none = digital ? '-' : (char)0xFA;
    for (int i = 0; i < 4; i++) { tmp[i] = (i < bars) ? full : none; }
    tmp[4] = '\0';
}

int osd_format_line(char *out, size_t out_cap, const char *tmpl,
                    const fc_state_t *fc, const cam_state_t *cam,
                    const cfg_t *cfg, bool blink_phase)
{
    return osd_format_line_rp(out, out_cap, tmpl, fc, cam, cfg, blink_phase, NULL);
}

int osd_format_line_rp(char *out, size_t out_cap, const char *tmpl,
                       const fc_state_t *fc, const cam_state_t *cam,
                       const cfg_t *cfg, bool blink_phase, const osd_rp_t *rp)
{
    size_t len = 0;
    out[0] = '\0';
    char tmp[24];
    int arrow_at[2] = { -1, -1 };   // indices of the %rp glyph placeholders in out (-1 = none)
    uint8_t arrow_glyph[2] = { 0, 0 };
    int bi_at = -1;                 // index of the %bi battery-icon placeholder (digital OSD), patched after the filter
    uint8_t bi_glyph = 0;

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
            batt_bar4(tmp, cam->battery_pct.v, cfg->osd.type == 1);
            len = app(out, out_cap, len, tmp);
        } else if (TOK2('b','i')) {                // battery icon (single glyph)
            p++;
            if (!field_on(cfg, OSD_TOK_BI) || !cam->battery_pct.valid) { continue; }
            // Betaflight MAX7456 font, verified @ BF drivers/osd_symbols.h
            // (tag 2025.12.5, commit 7348054): SYM_BATT_FULL 0x90 ..
            // SYM_BATT_EMPTY 0x96 -- a 7-step "evolution" set, exactly
            // one glyph per battery state.
            int lvl = (100 - (int)cam->battery_pct.v) * 7 / 100;
            if (lvl < 0) { lvl = 0; } else if (lvl > 6) { lvl = 6; }
            if (cfg->osd.type == 1) {
                // Digital OSD: the line filter below keeps only 0x20..0x5F, which used to turn this
                // token into the %bb bar ("#---"). The goggles' font does have Betaflight's battery
                // glyphs (Betaflight's own battery element draws them over the same display port), so
                // like the Remote ID arrow the glyph is patched in AFTER the filter and the clamp.
                // %bb stays the ASCII bar for anyone who prefers it.
                bi_at = (int)len;
                bi_glyph = (uint8_t)(0x90 + lvl);
                tmp[0] = RID_ARROW_PLACEHOLDER;    // '^': inside 0x20..0x5F, survives the filter
                tmp[1] = '\0';
            } else {
                tmp[0] = (char)(0x90 + lvl);
                tmp[1] = '\0';
            }
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
        } else if (TOK2('i','x')) {                // ISO max (GoPro setting 13, video only)
            p++;
            if (!field_on(cfg, OSD_TOK_IX) || !cam->iso_max.valid) { continue; }
            len = app(out, out_cap, len, cam->iso_max.v);
        } else if (TOK2('i','n')) {                // ISO min (setting 102)
            p++;
            if (!field_on(cfg, OSD_TOK_IN) || !cam->iso_min.valid) { continue; }
            len = app(out, out_cap, len, cam->iso_min.v);
        } else if (TOK2('s','h')) {                // shutter (setting 145), e.g. 1/120
            p++;
            if (!field_on(cfg, OSD_TOK_SH) || !cam->shutter.valid) { continue; }
            len = app(out, out_cap, len, cam->shutter.v);
        } else if (TOK2('w','b')) {                // white balance (setting 115)
            p++;
            if (!field_on(cfg, OSD_TOK_WB) || !cam->wb.valid) { continue; }
            len = app(out, out_cap, len, cam->wb.v);
        } else if (TOK2('c','o')) {                // Protune colour (setting 116)
            p++;
            if (!field_on(cfg, OSD_TOK_CO) || !cam->color.valid) { continue; }
            len = app(out, out_cap, len, cam->color.v);
        } else if (TOK2('s','a')) {                // sharpness (setting 117)
            p++;
            if (!field_on(cfg, OSD_TOK_SA) || !cam->sharpness.valid) { continue; }
            len = app(out, out_cap, len, cam->sharpness.v);
        } else if (TOK2('e','v')) {                // EV compensation (setting 118)
            p++;
            if (!field_on(cfg, OSD_TOK_EV) || !cam->ev.valid) { continue; }
            len = app(out, out_cap, len, cam->ev.v);
        } else if (TOK2('p','t')) {                // Protune ON/OFF (setting 114)
            p++;
            if (!field_on(cfg, OSD_TOK_PT) || !cam->protune.valid) { continue; }
            len = app(out, out_cap, len, cam->protune.v);
        } else if (TOK2('r','p')) {                // Remote ID proximity warning (spec 8.8.7)
            p++;
            if (!field_on(cfg, OSD_TOK_RP) || !rp || !rp->text[0]) { continue; }
            size_t at = len;
            len = app(out, out_cap, len, rp->text);
            for (int k = 0; k < 2; k++) {
                if (rp->pt.idx[k] >= 0 && at + (size_t)rp->pt.idx[k] < len) {
                    arrow_at[k] = (int)(at + (size_t)rp->pt.idx[k]);
                    arrow_glyph[k] = rp->pt.glyph[k];
                }
            }
        } else {
            // unknown token: emit literally
            if (len + 2 < out_cap) { out[len++] = '%'; out[len++] = a; out[len] = '\0'; }
        }
        #undef TOK2
    }

    // Digital OSD (DJI goggles): only the plain-ASCII slots 0x20..0x5F are
    // reliably present in the DJI font -- fold lowercase up, drop the rest
    // (graphical glyphs, control chars, {|}~` etc.). Analog leaves it alone.
    if (cfg->osd.type == 1) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)out[i];
            if (c >= 'a' && c <= 'z') { c -= 32; }
            if (c < 0x20 || c > 0x5F) { c = ' '; }
            out[i] = (char)c;
        }
    }

    // hard 16-char clamp (spec 9.1)
    if (len > 16) { out[16] = '\0'; len = 16; }

    // The Remote ID arrow (Betaflight's own home-arrow glyphs 0x60..0x6F) is exempt from the
    // digital filter (owner decision 2026-11-18, spec 9.1 / 8.8.7): patched in only now, after the
    // filter AND the clamp, and only if its placeholder survived the clamp.
    for (int k = 0; k < 2; k++) {
        if (arrow_at[k] >= 0 && (size_t)arrow_at[k] < len && out[arrow_at[k]] == RID_ARROW_PLACEHOLDER) {
            out[arrow_at[k]] = (char)arrow_glyph[k];
        }
    }
    if (bi_at >= 0 && (size_t)bi_at < len && out[bi_at] == RID_ARROW_PLACEHOLDER) {   // %bi on a digital OSD
        out[bi_at] = (char)bi_glyph;
    }
    return (int)len;
}

// ---- Remote ID line replacement (owner decision 2026-11-18) ---------------------------------
int osd_rp_line(const cfg_t *cfg)
{
    for (int i = 0; i < 4; i++) {
        char l[sizeof(cfg->osd.line[i]) + 1];
        memcpy(l, cfg->osd.line[i], sizeof(cfg->osd.line[i]));
        l[sizeof(cfg->osd.line[i])] = '\0';
        if (strstr(l, "%rp")) { return i; }
    }
    return -1;
}

void osd_compose_lines(char line[4][20], const fc_state_t *fc, const cam_state_t *cam,
                       const cfg_t *cfg, bool blink, const osd_rp_t *rp)
{
    int rl = osd_rp_line(cfg);
    // Disconnect display (spec 9.6): slot0 = CAM LOST / CAM ? , others blank.
    if (cam->link != CAM_LINK_READY) {
        bool searching = (cam->link == CAM_LINK_CONNECTING || cam->link == CAM_LINK_SYNCING ||
                          cam->link == CAM_LINK_SCAN);
        const char *s0 = searching ? (blink ? "" : "CAM ?") : "CAM LOST";
        snprintf(line[0], 20, "%s", s0);
        line[1][0] = line[2][0] = line[3][0] = '\0';
    } else {
        for (int i = 0; i < 4; i++) {
            osd_format_line_rp(line[i], 20, cfg->osd.line[i], fc, cam, cfg, blink, i == rl ? rp : NULL);
        }
    }
    // Replacement: AFTER the CAM LOST path, so it works during CAM LOST and with no camera. Not
    // tied to the %rp field-enable bit (rid.warn.enabled is the master, as the takeover was).
    if (rp && rp->replace && rl >= 0) {
        snprintf(line[rl], 20, "%s", rp->text);
        for (int k = 0; k < 2; k++) {
            int ai = rp->pt.idx[k];
            if (ai >= 0 && ai < 16 && line[rl][ai] == RID_ARROW_PLACEHOLDER) {
                line[rl][ai] = (char)rp->pt.glyph[k];
            }
        }
    }
}
