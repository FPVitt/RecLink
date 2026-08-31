// json_cfg.c -- cfg_t <-> JSON. GET returns everything (password redacted);
// POST is a partial merge onto a snapshot, then cfg_commit validates/clamps.
#include "portal_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void addr_to_str(const uint8_t a[6], char *s, size_t n)
{
    snprintf(s, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

cJSON *cfg_to_json(const cfg_t *c)
{
    cJSON *r = cJSON_CreateObject();

    cJSON *cam = cJSON_AddObjectToObject(r, "camera");
    cJSON_AddStringToObject(cam, "brand", c->camera.brand);
    cJSON_AddBoolToObject(cam, "bonded", c->camera.bonded);
    char addr[20]; addr_to_str(c->camera.addr, addr, sizeof(addr));
    cJSON_AddStringToObject(cam, "addr", addr);
    cJSON_AddStringToObject(cam, "name", c->camera.name);
    cJSON_AddNumberToObject(cam, "dji_device_id", c->camera.dji_device_id);

    cJSON *rec = cJSON_AddObjectToObject(r, "record");
    cJSON_AddNumberToObject(rec, "mode", c->record.mode);
    cJSON_AddNumberToObject(rec, "arm_delay_ms", c->record.arm_delay_ms);
    cJSON_AddNumberToObject(rec, "stop_delay_ms", c->record.stop_delay_ms);
    cJSON_AddNumberToObject(rec, "verify_timeout_ms", c->record.verify_timeout_ms);
    cJSON_AddNumberToObject(rec, "retry_count", c->record.retry_count);
    cJSON *sw = cJSON_AddObjectToObject(rec, "switch");
    cJSON_AddNumberToObject(sw, "channel", c->record.sw.channel);
    cJSON_AddNumberToObject(sw, "threshold_us", c->record.sw.threshold_us);

    cJSON *pre = cJSON_AddObjectToObject(r, "preset");
    cJSON_AddBoolToObject(pre, "enabled", c->preset.enabled);
    cJSON_AddNumberToObject(pre, "channel", c->preset.channel);
    cJSON_AddNumberToObject(pre, "dwell_ms", c->preset.dwell_ms);
    cJSON_AddBoolToObject(pre, "allow_during_record", c->preset.allow_during_record);
    cJSON *bands = cJSON_AddArrayToObject(pre, "bands");
    for (int i = 0; i < 3; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "max_us", c->preset.bands[i].max_us);
        cJSON_AddNumberToObject(b, "id", c->preset.bands[i].id);
        cJSON_AddItemToArray(bands, b);
    }

    cJSON *osd = cJSON_AddObjectToObject(r, "osd");
    cJSON_AddNumberToObject(osd, "refresh_hz", c->osd.refresh_hz);
    cJSON_AddNumberToObject(osd, "type", c->osd.type);
    cJSON_AddNumberToObject(osd, "corner", c->osd.corner);
    cJSON *lines = cJSON_AddArrayToObject(osd, "lines");
    for (int i = 0; i < 4; i++) { cJSON_AddItemToArray(lines, cJSON_CreateString(c->osd.line[i])); }
    cJSON_AddNumberToObject(osd, "fields_mask", c->osd.fields_mask);
    cJSON *warn = cJSON_AddObjectToObject(osd, "warn");
    cJSON_AddNumberToObject(warn, "batt_pct", c->osd.warn.batt_pct);
    cJSON_AddNumberToObject(warn, "sd_low_min", c->osd.warn.sd_low_min);
    cJSON_AddBoolToObject(warn, "blink", c->osd.warn.blink);
    cJSON_AddBoolToObject(warn, "show_on_lost", c->osd.warn.show_on_lost);
    cJSON_AddNumberToObject(osd, "units", c->osd.units);

    cJSON *clk = cJSON_AddObjectToObject(r, "clock");
    cJSON_AddBoolToObject(clk, "enabled", c->clock.enabled);
    cJSON_AddBoolToObject(clk, "require_gps", c->clock.require_gps);
    cJSON_AddNumberToObject(clk, "min_sats", c->clock.min_sats);
    cJSON_AddNumberToObject(clk, "threshold_s", c->clock.threshold_s);
    cJSON_AddNumberToObject(clk, "utc_offset_min", c->clock.utc_offset_min);
    cJSON_AddBoolToObject(clk, "dst", c->clock.dst);
    cJSON_AddNumberToObject(clk, "check_period_min", c->clock.check_period_min);
    cJSON_AddNumberToObject(clk, "resync_period_min", c->clock.resync_period_min);
    cJSON_AddNumberToObject(clk, "retry_count", c->clock.retry_count);

    cJSON *gp = cJSON_AddObjectToObject(r, "gps_push");
    cJSON_AddBoolToObject(gp, "enabled", c->gps_push.enabled);
    cJSON_AddNumberToObject(gp, "rate_hz", c->gps_push.rate_hz);

    cJSON_AddNumberToObject(cJSON_AddObjectToObject(r, "hilight"), "channel", c->hilight.channel);

    cJSON *msp = cJSON_AddObjectToObject(r, "msp");
    cJSON_AddNumberToObject(msp, "baud", c->msp.baud);
    cJSON_AddNumberToObject(msp, "swap_pins", c->msp_swap_pins);

    cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", c->wifi.ssid);
    cJSON_AddStringToObject(wifi, "password", "");   // redacted (spec: server authoritative)
    cJSON_AddNumberToObject(wifi, "channel", c->wifi.channel);
    cJSON_AddNumberToObject(wifi, "portal_trigger", c->wifi.portal_trigger);
    cJSON_AddNumberToObject(wifi, "portal_timeout_min", c->wifi.portal_timeout_min);

    cJSON_AddNumberToObject(r, "led_brightness", c->led_brightness);
    cJSON_AddNumberToObject(r, "ui_flags", c->ui_flags);
    return r;
}

// ---- merge helpers ------------------------------------------------
#define J_OBJ(parent, key)  cJSON_GetObjectItemCaseSensitive((parent), (key))
// dst may point at a packed (unaligned) cfg_t member -- always memcpy, never
// dereference a typed pointer into the blob (misaligned store -> panic on Xtensa).
// Accept a JSON number OR a numeric string ("1"): the portal's <select>
// elements post their value as a string, and dropdown settings were silently
// dropped when this only took numbers.
static bool jnum(const cJSON *it, double *out)
{
    if (cJSON_IsNumber(it)) { *out = it->valuedouble; return true; }
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        char *end = NULL;
        double v = strtod(it->valuestring, &end);
        if (end && *end == '\0') { *out = v; return true; }
    }
    return false;
}
static void mnum_u(const cJSON *o, const char *k, void *dst, size_t sz)
{
    double v;
    if (!jnum(J_OBJ(o, k), &v)) { return; }
    if (v < 0) { v = 0; }
    switch (sz) {
    case 1: { uint8_t  t = (uint8_t)v;  memcpy(dst, &t, 1); break; }
    case 2: { uint16_t t = (v > 65535.0) ? 0xFFFF : (uint16_t)v; memcpy(dst, &t, 2); break; }
    case 4: { uint32_t t = (v > 4294967295.0) ? 0xFFFFFFFFu : (uint32_t)v; memcpy(dst, &t, 4); break; }
    }
}
static void mnum_i16(const cJSON *o, const char *k, void *dst)
{
    double v;
    if (!jnum(J_OBJ(o, k), &v)) { return; }
    int16_t t = (int16_t)v;
    memcpy(dst, &t, sizeof(t));
}
static void mbool(const cJSON *o, const char *k, uint8_t *dst)
{
    const cJSON *it = J_OBJ(o, k);
    double v;
    if (cJSON_IsBool(it)) { *dst = cJSON_IsTrue(it) ? 1 : 0; }
    else if (jnum(it, &v)) { *dst = v != 0; }
}
static void mstr(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *it = J_OBJ(o, k);
    if (cJSON_IsString(it) && it->valuestring) { strlcpy(dst, it->valuestring, cap); }
}

esp_err_t json_merge_cfg(const cJSON *j, cfg_t *c, char *err, size_t errlen)
{
    if (err && errlen) { err[0] = '\0'; }
    const cJSON *o;

    if ((o = J_OBJ(j, "record"))) {
        mnum_u(o, "mode", &c->record.mode, 1);
        mnum_u(o, "arm_delay_ms", &c->record.arm_delay_ms, 2);
        mnum_u(o, "stop_delay_ms", &c->record.stop_delay_ms, 2);
        mnum_u(o, "verify_timeout_ms", &c->record.verify_timeout_ms, 2);
        mnum_u(o, "retry_count", &c->record.retry_count, 1);
        const cJSON *sw = J_OBJ(o, "switch");
        if (sw) {
            mnum_u(sw, "channel", &c->record.sw.channel, 1);
            mnum_u(sw, "threshold_us", &c->record.sw.threshold_us, 2);
        }
    }
    if ((o = J_OBJ(j, "preset"))) {
        mbool(o, "enabled", &c->preset.enabled);
        mnum_u(o, "channel", &c->preset.channel, 1);
        mnum_u(o, "dwell_ms", &c->preset.dwell_ms, 2);
        mbool(o, "allow_during_record", &c->preset.allow_during_record);
        const cJSON *bands = J_OBJ(o, "bands");
        if (cJSON_IsArray(bands)) {
            for (int i = 0; i < 3 && i < cJSON_GetArraySize(bands); i++) {
                const cJSON *b = cJSON_GetArrayItem(bands, i);
                mnum_u(b, "max_us", &c->preset.bands[i].max_us, 2);
                mnum_u(b, "id", &c->preset.bands[i].id, 4);
            }
        }
    }
    if ((o = J_OBJ(j, "osd"))) {
        mnum_u(o, "refresh_hz", &c->osd.refresh_hz, 1);
        mnum_u(o, "type", &c->osd.type, 1);
        mnum_u(o, "corner", &c->osd.corner, 1);
        mnum_u(o, "fields_mask", &c->osd.fields_mask, 4);
        mnum_u(o, "units", &c->osd.units, 1);
        const cJSON *lines = J_OBJ(o, "lines");
        if (cJSON_IsArray(lines)) {
            for (int i = 0; i < 4 && i < cJSON_GetArraySize(lines); i++) {
                const cJSON *l = cJSON_GetArrayItem(lines, i);
                if (cJSON_IsString(l)) { strlcpy(c->osd.line[i], l->valuestring, sizeof(c->osd.line[i])); }
            }
        }
        const cJSON *w = J_OBJ(o, "warn");
        if (w) {
            mnum_u(w, "batt_pct", &c->osd.warn.batt_pct, 1);
            mnum_u(w, "sd_low_min", &c->osd.warn.sd_low_min, 1);
            mbool(w, "blink", &c->osd.warn.blink);
            mbool(w, "show_on_lost", &c->osd.warn.show_on_lost);
        }
    }
    if ((o = J_OBJ(j, "clock"))) {
        mbool(o, "enabled", &c->clock.enabled);
        mbool(o, "require_gps", &c->clock.require_gps);
        mnum_u(o, "min_sats", &c->clock.min_sats, 1);
        mnum_u(o, "threshold_s", &c->clock.threshold_s, 2);
        mnum_i16(o, "utc_offset_min", &c->clock.utc_offset_min);
        mbool(o, "dst", &c->clock.dst);
        mnum_u(o, "check_period_min", &c->clock.check_period_min, 1);
        mnum_u(o, "resync_period_min", &c->clock.resync_period_min, 2);
        mnum_u(o, "retry_count", &c->clock.retry_count, 1);
    }
    if ((o = J_OBJ(j, "gps_push"))) {
        mbool(o, "enabled", &c->gps_push.enabled);
        mnum_u(o, "rate_hz", &c->gps_push.rate_hz, 1);
    }
    if ((o = J_OBJ(j, "hilight"))) { mnum_u(o, "channel", &c->hilight.channel, 1); }
    if ((o = J_OBJ(j, "msp"))) {
        mnum_u(o, "baud", &c->msp.baud, 4);
        mbool(o, "swap_pins", &c->msp_swap_pins);
    }
    if ((o = J_OBJ(j, "wifi"))) {
        mstr(o, "ssid", c->wifi.ssid, sizeof(c->wifi.ssid));
        const cJSON *pw = J_OBJ(o, "password");
        if (cJSON_IsString(pw) && pw->valuestring && pw->valuestring[0]) {
            strlcpy(c->wifi.password, pw->valuestring, sizeof(c->wifi.password));
        }
        mnum_u(o, "channel", &c->wifi.channel, 1);
        mnum_u(o, "portal_trigger", &c->wifi.portal_trigger, 1);
        mnum_u(o, "portal_timeout_min", &c->wifi.portal_timeout_min, 1);
    }
    mnum_u(j, "led_brightness", &c->led_brightness, 1);
    mnum_u(j, "ui_flags", &c->ui_flags, 1);
    return ESP_OK;
}
