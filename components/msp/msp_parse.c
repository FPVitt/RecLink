#include "msp_parse.h"
#include "msp.h"
#include "monoclock.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "msp.parse";

// ---- little-endian readers with bounds check ---------------------------
#define RD_GUARD(need) do { if ((size_t)(off + (need)) > f->len) return changed; } while (0)

static uint8_t  rd_u8 (const uint8_t *p, size_t o) { return p[o]; }
static uint16_t rd_u16(const uint8_t *p, size_t o) { return (uint16_t)(p[o] | (p[o+1] << 8)); }
static uint32_t rd_u32(const uint8_t *p, size_t o) {
    return (uint32_t)p[o] | ((uint32_t)p[o+1] << 8) | ((uint32_t)p[o+2] << 16) | ((uint32_t)p[o+3] << 24);
}
static int32_t  rd_i32(const uint8_t *p, size_t o) { return (int32_t)rd_u32(p, o); }

void msp_parse_ctx_init(msp_parse_ctx_t *c, uint8_t min_sats)
{
    memset(c, 0, sizeof(*c));
    c->arm_bit_index = -1;
    c->min_sats = min_sats ? min_sats : 6;
}

// RTC plausibility: after 2025-01-01, before 2100.
static bool epoch_plausible(int32_t e) { return e > 1735689600 && e < 4102444800; }

static void recompute_gps_good(fc_state_t *s, uint8_t min_sats)
{
    bool good = (s->gps.fix >= 3) && (s->gps.sats >= min_sats);
    if (good && !s->gps_good) { s->gps_good_since_ms = now_ms(); }
    if (!good) { s->gps_good_since_ms = 0; }
    s->gps_good = good;
}

bool msp_parse_frame(const msp_frame_t *f, fc_state_t *s, msp_parse_ctx_t *ctx)
{
    bool changed = false;
    const uint8_t *p = f->payload;
    size_t off = 0;

    if (f->dir == '!') {
        ESP_LOGW(TAG, "FC error resp for func %u", f->func);
        return false;
    }

    switch (f->func) {
    case MSP_API_VERSION:
        RD_GUARD(3);
        s->api_major = rd_u8(p, 1);
        s->api_minor = rd_u8(p, 2);
        // OSD custom messages need MSP API >= 1.47 (spec 1.3).
        s->osd_supported = (s->api_major > 1) || (s->api_major == 1 && s->api_minor >= 47);
        changed = true;
        break;

    case MSP_FC_VARIANT:
        RD_GUARD(4);
        memcpy(s->fc_variant, p, 4);
        s->fc_variant[4] = '\0';
        changed = true;
        break;

    case MSP_BOXIDS: {
        // Array of permanent box IDs in flightModeFlags bit order. BOXARM == 0.
        ctx->arm_bit_index = -1;
        for (size_t i = 0; i < f->len && i < 32; i++) {
            if (p[i] == 0) { ctx->arm_bit_index = (int)i; break; }
        }
        ctx->boxids_ok = (ctx->arm_bit_index >= 0);
        if (!ctx->boxids_ok) { ESP_LOGW(TAG, "BOXARM not found in %u boxids", f->len); }
        changed = true;
        break;
    }

    case MSP_STATUS: {
        // spec 4.3 -- VERIFY offsets against src/main/msp/msp.c for the target
        // build. Fixed prefix:
        //   u16 task_delta | u16 i2c_err | u16 sensor_flags | u32 flightModeFlags
        //   u8 pid_profile | u16 avg_load ...
        RD_GUARD(11);
        uint16_t sensors = rd_u16(p, 4);
        uint32_t modeflags = rd_u32(p, 6);
        s->gps_present = (sensors >> 3) & 1;

        if (ctx->arm_bit_index >= 0) {
            s->armed = (modeflags >> ctx->arm_bit_index) & 1u;
        }
        // Arming-disable flags live further in the extended payload; their exact
        // offset is version-specific (VERIFY). Best-effort: last u32 before the
        // trailing config-state byte, when the payload is long enough.
        if (f->len >= 22) {
            s->arming_disable_flags = rd_u32(p, f->len >= 23 ? f->len - 5 : f->len - 4);
        }
        changed = true;
        break;
    }

    case MSP_RC: {
        uint8_t n = (uint8_t)(f->len / 2);
        if (n > 18) { n = 18; }
        for (uint8_t i = 0; i < n; i++) { s->rc[i] = rd_u16(p, i * 2); }
        s->rc_channel_count = n;
        changed = true;
        break;
    }

    case MSP_RAW_GPS: {
        RD_GUARD(16);
        uint8_t fix  = rd_u8(p, 0);
        // Betaflight: 0 no fix, 1 = 2D-ish, 2 = 3D. Normalise to 0/2/3.
        s->gps.fix   = (fix >= 2) ? 3 : (fix == 1 ? 2 : 0);
        s->gps.sats  = rd_u8(p, 1);
        s->gps.lat_1e7 = rd_i32(p, 2);
        s->gps.lon_1e7 = rd_i32(p, 6);
        s->gps.alt_m   = rd_u16(p, 10);
        s->gps.spd_cm_s = rd_u16(p, 12);
        s->gps.course_ddeg = rd_u16(p, 14);
        recompute_gps_good(s, ctx->min_sats);
        changed = true;
        break;
    }

    case MSP_ANALOG:
        // rssi optional; skip unless needed.
        break;

    case MSP_BATTERY_STATE: {
        // spec 16.A: cells u8, cap u16, vbat u8, mAh u16, amps i16, state u8, vbat u16
        RD_GUARD(9);
        s->batt_state = rd_u8(p, 8);
        // crude % estimate from cell voltage is unreliable; leave batt_pct_est
        // for MSP_ANALOG-derived logic later. Keep state only.
        changed = true;
        break;
    }

    case MSP_OSD_CANVAS:
        RD_GUARD(2);
        s->osd_cols = rd_u8(p, 0);
        s->osd_rows = rd_u8(p, 1);
        changed = true;
        break;

    case MSP_RTC: {
        RD_GUARD(4);
        int32_t secs = rd_i32(p, 0);
        uint16_t millis = (f->len >= 6) ? rd_u16(p, 4) : 0;
        s->rtc_epoch = secs;
        s->rtc_millis = millis;
        s->rtc_sample_uptime_ms = now_ms();
        s->rtc_valid = epoch_plausible(secs);
        changed = true;
        break;
    }

    case MSP2_GET_TEXT: {
        // [type u8][len u8][chars]; we only ask for type 2 (craft).
        RD_GUARD(2);
        uint8_t type = rd_u8(p, 0);
        uint8_t n = rd_u8(p, 1);
        if (type == 2 && (size_t)(2 + n) <= f->len) {
            if (n > 16) { n = 16; }
            memcpy(s->craft_name, p + 2, n);
            s->craft_name[n] = '\0';
            changed = true;
        }
        break;
    }

    default:
        break;
    }

    return changed;
}
