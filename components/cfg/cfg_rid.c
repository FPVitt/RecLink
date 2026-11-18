// cfg_rid.c -- the cfg v7 Remote ID tail (spec 8.8.9 / 11.1): defaults, its own CRC, validation.
// Pure (no NVS, no RTOS, no logging) so the host tests compile it directly.
#include "cfg.h"
#include "crc.h"

#include <stddef.h>
#include <string.h>

#define CLAMP(x, lo, hi) do { if ((x) < (lo)) (x) = (lo); else if ((x) > (hi)) (x) = (hi); } while (0)

void cfg_rid_defaults(cfg_t *c)
{
    memset(&c->rid, 0, sizeof(c->rid));
    c->rid.enabled   = 0;                 // off until bench-proven (spec 8.8.13 #2)
    c->rid.mode      = RID_MODE_BLE;
    c->rid.ble_duty  = RID_DUTY_NORMAL;
    c->rid.ble_coded = 0;
    c->rid.stale_s   = 5;
    c->rid.min_sats  = 6;
    c->rid.warn.enabled        = 1;
    c->rid.warn.notice_m       = 1000;
    c->rid.warn.warn_m         = 300;
    c->rid.warn.alert_m        = 100;
    c->rid.warn.blink          = 1;
    c->rid.warn.takeover       = 1;
    c->rid.warn.takeover_slot  = 1;      // takeover / takeover_slot: RESERVED (ignored), kept for the layout
    c->rid.warn.takeover_level = 2;      // ALERT
    c->rid.warn.bearing        = 0;      // auto
    c->rid.warn.ground         = 0;
    cfg_rid_seal(c);
}

uint32_t cfg_rid_crc(const cfg_t *c)
{
    return crc32_iso_hdlc((const uint8_t *)&c->rid, offsetof(typeof(c->rid), rid_crc32));
}

void cfg_rid_seal(cfg_t *c)
{
    uint32_t crc = cfg_rid_crc(c);
    memcpy(&c->rid.rid_crc32, &crc, sizeof(crc));    // packed member: no typed store
}

bool cfg_rid_check(cfg_t *c)
{
    uint32_t stored;
    memcpy(&stored, &c->rid.rid_crc32, sizeof(stored));
    if (stored == cfg_rid_crc(c)) { return true; }
    cfg_rid_defaults(c);                              // disabled: a bit-flip must not enable RX
    return false;
}

static bool printable_ascii(const char *s)
{
    for (int i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7E) { return false; }
    }
    return true;
}

void cfg_rid_validate(cfg_t *c)
{
    CLAMP(c->rid.enabled, 0, 1);
    CLAMP(c->rid.mode, RID_MODE_BLE, RID_MODE_BOTH);
    CLAMP(c->rid.ble_duty, RID_DUTY_LOW, RID_DUTY_HIGH);
    c->rid.ble_coded = 0;                                    // reserved in v1
    CLAMP(c->rid.stale_s, 2, 30);
    // A Location arrives at >= 1 Hz; at LOW duty ~59 % of 5 s windows hear none (spec 8.8.3).
    if (c->rid.ble_duty == RID_DUTY_LOW && c->rid.stale_s < 10) { c->rid.stale_s = 10; }
    CLAMP(c->rid.min_sats, 5, 12);       // BF does not correct yaw with <= 4 satellites (8.8.5)

    CLAMP(c->rid.warn.enabled, 0, 1);
    CLAMP(c->rid.warn.blink, 0, 1);
    CLAMP(c->rid.warn.takeover, 0, 1);
    CLAMP(c->rid.warn.ground, 0, 1);
    CLAMP(c->rid.warn.takeover_slot, 0, 3);
    CLAMP(c->rid.warn.takeover_level, 1, 2);
    CLAMP(c->rid.warn.bearing, 0, 3);
    // 5000 >= notice_m >= warn_m >= alert_m >= 20: each clamped, then re-ordered (not rejected).
    uint16_t t[3] = { c->rid.warn.notice_m, c->rid.warn.warn_m, c->rid.warn.alert_m };
    for (int i = 0; i < 3; i++) { CLAMP(t[i], 20, 5000); }
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 2 - i; k++) {
            if (t[k] < t[k + 1]) { uint16_t x = t[k]; t[k] = t[k + 1]; t[k + 1] = x; }
        }
    }
    c->rid.warn.notice_m = t[0];
    c->rid.warn.warn_m   = t[1];
    c->rid.warn.alert_m  = t[2];

    // Ignore list: NUL-terminated printable ASCII, duplicates dropped, compacted.
    for (int i = 0; i < RID_IGNORE_MAX; i++) {
        c->rid.ignore[i][RID_ID_LEN - 1] = '\0';
        if (!printable_ascii(c->rid.ignore[i])) { memset(c->rid.ignore[i], 0, RID_ID_LEN); }
        for (int k = 0; k < i; k++) {
            if (c->rid.ignore[i][0] && strcmp(c->rid.ignore[i], c->rid.ignore[k]) == 0) {
                memset(c->rid.ignore[i], 0, RID_ID_LEN);
            }
        }
    }
    for (int i = 0, w = 0; i < RID_IGNORE_MAX; i++) {
        if (!c->rid.ignore[i][0]) { continue; }
        if (w != i) {
            memcpy(c->rid.ignore[w], c->rid.ignore[i], RID_ID_LEN);
            memset(c->rid.ignore[i], 0, RID_ID_LEN);
        }
        w++;
    }
    for (int i = 0; i < RID_IGNORE_MAX; i++) {               // clear any bytes after the NUL
        size_t n = strnlen(c->rid.ignore[i], RID_ID_LEN);
        if (n < RID_ID_LEN) { memset(c->rid.ignore[i] + n, 0, RID_ID_LEN - n); }
    }
}
