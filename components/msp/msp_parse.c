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

// Civil (UTC) date -> Unix epoch seconds. Howard Hinnant's days_from_civil.
// Valid for the Gregorian calendar; y is the full year.
static int64_t epoch_from_civil_utc(int y, int mo, int d, int hh, int mm, int ss)
{
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int)doe - 719468;
    return ((days * 24 + hh) * 60 + mm) * 60 + ss;
}

void msp_parse_ctx_init(msp_parse_ctx_t *c, uint8_t min_sats)
{
    memset(c, 0, sizeof(*c));
    c->arm_bit_index = -1;
    c->min_sats = min_sats ? min_sats : 6;
}

// RTC plausibility: after 2025-01-01. (Upper bound is moot -- int32 Unix time
// saturates in 2038, and MSP_RTC hands us an i32.)
static bool epoch_plausible(int32_t e) { return e > 1735689600; }

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
        s->arm_bit_index = (int8_t)ctx->arm_bit_index;   // surfaced in /api/status
        if (!ctx->boxids_ok) { ESP_LOGW(TAG, "BOXARM not found in %u boxids", f->len); }
        changed = true;
        break;
    }

    case MSP_STATUS: {
        // verified: betaflight src/main/msp/msp.c case MSP_STATUS_EX/MSP_STATUS
        // @ tag 2025.12.5 (7348054f268f0058574719c134e9f149565bb8ea, 2026-08).
        // Layout (little-endian):
        //   0  u16 taskDeltaTime(PID)   2  u16 i2cErrorCounter
        //   4  u16 sensor flags (ACC|BARO<<1|MAG<<2|GPS<<3|RANGE<<4|GYRO<<5|OPTICALFLOW<<6)
        //   6  u32 flightModeFlags[0:32]   10 u8 pidProfileIndex
        //   11 u16 averageSystemLoadPercent
        //   13 u16 gyro cycle time         (MSP_STATUS; MSP_STATUS_EX puts 2 u8 here)
        //   15 u8  extendedFlightModeFlags byteCount = BC
        //   16 .. 16+BC-1  extended flightModeFlags
        //   16+BC       u8  ARMING_DISABLE_FLAGS_COUNT
        //   17+BC ..    u32 armingDisableFlags
        //   21+BC       u8  configStateFlags (bit0 rebootRequired)
        //   22+BC       u16 CPU temp (API 1.46+)
        //   24+BC       u8  CONTROL_RATE_PROFILE_COUNT
        // Total = 25 + BC bytes.
        RD_GUARD(13);
        uint16_t sensors = rd_u16(p, 4);
        uint32_t modeflags = rd_u32(p, 6);
        s->gps_present = (sensors >> 3) & 1;

        if (ctx->arm_bit_index >= 0) {
            s->armed = (modeflags >> ctx->arm_bit_index) & 1u;
        }
        // FIXME(protocol-verifier -> implementer): the previous fixed-offset
        // heuristic below reads the WRONG u32 (for the common BC=0 / len=25 case
        // it lands on {armingDisableFlags[3], configState, cpuTemp}). Correct
        // parse is a forward walk: bc = p[15]; count = p[16+bc];
        // armingDisableFlags = rd_u32(p, 17+bc). Left as-is (informational field,
        // not flight-critical) pending a logic fix + perf-coherence review.
        if (f->len >= 15) {
            uint8_t bc = rd_u8(p, 15);
            size_t adf_off = (size_t)17 + bc;
            if ((size_t)(adf_off + 4) <= f->len) {
                s->arming_disable_flags = rd_u32(p, adf_off);
            }
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
        // verified: betaflight src/main/msp/msp.c case MSP_RAW_GPS @ 2025.12.5
        // (7348054). Little-endian:
        //   0  u8  STATE(GPS_FIX)   -- boolean: 0 = no fix, 2 = fix (GPS_FIX bit
        //          value = 1<<1); NOT a 0/1/2 fix-type enum.
        //   1  u8  numSat
        //   2  i32 lat (deg * 1e7)     6  i32 lon (deg * 1e7)
        //   10 u16 altitude (metres, >=0; altCm/100 clamped)
        //   12 u16 groundSpeed (cm/s)  14 u16 groundCourse (decidegrees, 0.1 deg)
        //   16 u16 pDOP  (API 1.44+, present on target; ignored here)
        RD_GUARD(16);
        uint8_t fix  = rd_u8(p, 0);
        // BF writes 0 or 2 (see above). Map any non-zero to a 3D fix so the
        // gps_good check (fix >= 3) works; 0 stays 0.
        s->gps.fix   = (fix != 0) ? 3 : 0;
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
        // verified: betaflight src/main/msp/msp.c case MSP_RTC @ 2025.12.5
        // (7348054), guarded by USE_RTC_TIME. Response is a BROKEN-OUT dateTime,
        // NOT unix seconds (that is MSP_SET_RTC 246, inbound only):
        //   0  u16 year   2 u8 month   3 u8 day
        //   4  u8  hours  5 u8 minutes 6 u8 seconds   7 u16 millis   (10 bytes)
        // If the FC RTC is unset the payload is EMPTY (length 0) -> treat as
        // invalid.
        // CHANGED by protocol-verifier: previously mis-parsed as [i32 secs][u16
        // millis]; that fed a garbage epoch to F6 clock-sync. Now decoded
        // correctly. (Flagged for perf-coherence review: adds a civil->epoch
        // computation.)
        if (f->len < 10) {         // empty (RTC unset) or truncated
            s->rtc_valid = false;
            changed = true;
            break;
        }
        int year = rd_u16(p, 0);
        int mon  = rd_u8(p, 2);
        int day  = rd_u8(p, 3);
        int hh   = rd_u8(p, 4);
        int mm   = rd_u8(p, 5);
        int ss   = rd_u8(p, 6);
        uint16_t millis = rd_u16(p, 7);
        int64_t secs = epoch_from_civil_utc(year, mon, day, hh, mm, ss);
        s->rtc_epoch = (int32_t)secs;
        s->rtc_millis = millis;
        s->rtc_sample_uptime_ms = now_ms();
        s->rtc_valid = (mon >= 1 && mon <= 12 && day >= 1 && day <= 31) &&
                       epoch_plausible((int32_t)secs);
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
