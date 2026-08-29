// gopro_driver.c -- Open GoPro BLE driver (spec 6). Runs entirely in cam_task
// context. Every camera write is fire-and-forget (spec 1.2.4); results surface
// asynchronously via status/setting notifications.
#include "camera_gopro.h"
#include "gopro_tlv.h"
#include "gopro_enums.h"

#include <string.h>
#include <time.h>

#include "cfg.h"
#include "cam_state.h"
#include "ble_central.h"
#include "monoclock.h"
#include "applog.h"
#include "esp_log.h"
#include "host/ble_uuid.h"

static const char *TAG = "gopro";

// ---- GATT (spec 6.2). Base b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b -----------
// verified: Open GoPro BLE spec 2.0, gopro/OpenGoPro
//   models/constants/uuids.py @ main 0f963572 (2026-04-15) and
//   docs/ble/protocol/data_protocol @ gh-pages 616bfb80 (2026-06-08):
//   GOPRO_BASE_UUID = "b5f9{}-aa8d-11e3-9046-0002a5d5c51b"
//   S_CONTROL_QUERY 0000fea6-...  CQ_COMMAND 0072 / _RESP 0073
//   CQ_SETTINGS 0074 / _RESP 0075   CQ_QUERY 0076 / _RESP 0077
//   (NimBLE 128-bit UUID literals are little-endian byte order.)
#define GP_UUID(yy) BLE_UUID128_INIT(0x1b,0xc5,0xd5,0xa5,0x02,0x00,0x46,0x90, \
                                     0xe3,0x11,0x8d,0xaa,(yy),0x00,0xf9,0xb5)
static const ble_uuid128_t U_CMD_REQ    = GP_UUID(0x72);
static const ble_uuid128_t U_CMD_RSP    = GP_UUID(0x73);
static const ble_uuid128_t U_SET_REQ    = GP_UUID(0x74);
static const ble_uuid128_t U_SET_RSP    = GP_UUID(0x75);
static const ble_uuid128_t U_QRY_REQ    = GP_UUID(0x76);
static const ble_uuid128_t U_QRY_RSP    = GP_UUID(0x77);

// GoPro Control & Query service 0xFEA6.
#define GP_SVC_UUID16 0xFEA6

// ---- status / setting IDs -----------------------------------------------
// verified: Open GoPro spec 2.0 -- models/constants/statuses.py (StatusId) +
// settings.py (SettingId) @ gopro/OpenGoPro main 0f963572 (2026-04-15):
//   2 INTERNAL_BATTERY_BARS  6 OVERHEATING  8 BUSY  10 ENCODING
//   33 PRIMARY_STORAGE  35 REMAINING_VIDEO_TIME  54 SD_CARD_REMAINING (KB)
//   70 INTERNAL_BATTERY_PERCENTAGE
//   setting 2 VIDEO_RESOLUTION  3 FRAMES_PER_SECOND  121 VIDEO_LENS
enum { ST_BATT_BARS = 2, ST_HOT = 6, ST_BUSY = 8, ST_ENCODING = 10,
       ST_SD_STATUS = 33, ST_REMAIN_VIDEO_S = 35, ST_SD_REMAIN_KB = 54, ST_BATT_PCT = 70,
       ST_ACTIVE_PRESET = 89 };   // Current Preset ID (VERIFY.md GOPRO-STATUS-89)
enum { SET_RESOLUTION = 2, SET_FPS = 3, SET_LENS = 121 };

// ---- driver state ---------------------------------------------------
static const camera_cbs_t *CB;
static uint16_t h_cmd, h_set, h_qry;      // write value handles
static uint16_t vh_cmd_rsp, vh_set_rsp, vh_qry_rsp;
static gopro_reasm_t rz_cmd, rz_set, rz_qry;
static uint8_t  keepalive_div;
static uint32_t rec_start_ms;
static uint8_t  getall_retry;
static uint32_t last_getall_ms;      // get-all-settings poll
static uint32_t last_status_poll_ms; // get-all-status poll

static void gp_pull_all(void);
static bool     want_record_ack;
static bool     want_record_target;

// ---------------------------------------------------------------------------
static int64_t epoch_from_civil_utc(int y, int m, int d, int hh, int mm, int ss)
{
    // days from 1970-01-01 (Howard Hinnant's algorithm)
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int)doe - 719468;
    return ((days * 24 + hh) * 60 + mm) * 60 + ss;
}

// ---- low-level writers -------------------------------------------
static void wr(uint16_t handle, const uint8_t *body, uint8_t n)
{
    if (!handle) { return; }
    uint8_t pkt[64];
    size_t len = gopro_pack(pkt, sizeof(pkt), body, n);
    if (len) { ble_central_write(handle, pkt, len, true); }
}

// ---- driver ops -------------------------------------------------
// verified @ gopro/OpenGoPro 0f963572 / gh-pages 616bfb80 (Open GoPro spec 2.0):
//   CmdId.SET_SHUTTER=0x01 (param u8 0/1), TAG_HILIGHT=0x18 (no param),
//   LOAD_PRESET=0x40 (param u32 BIG-ENDIAN), LOAD_PRESET_GROUP=0x3E (u16 BE),
//   query 0x53 register statuses / 0x52 register settings (uint8 ID array),
//   0x13 get-all statuses / 0x12 get-all settings, push notif IDs 0x93/0x92.
//   Keep-alive = write SettingId.LED(91)=value 66 to CQ_SETTINGS: 5B 01 42
//   (gopro_pack prepends the 03 length byte). SDK default interval 3 s (<10 s).
static esp_err_t gp_record(bool start)
{
    uint8_t b[3] = { 0x01, 0x01, start ? 0x01 : 0x00 };
    wr(h_cmd, b, 3);
    want_record_ack = true;
    want_record_target = start;
    return ESP_OK;
}

static esp_err_t gp_select_preset(uint8_t slot)
{
    const cfg_t *c = cfg_get();
    if (slot > 2) { return ESP_ERR_INVALID_ARG; }
    uint32_t id = c->preset.bands[slot].id;
    uint8_t b[6] = { 0x40, 0x04, id >> 24, id >> 16, id >> 8, id };
    wr(h_cmd, b, 6);
    return ESP_OK;
}

static esp_err_t gp_set_datetime(int64_t utc_ms, int16_t off_min, bool dst)
{
    time_t local = (time_t)(utc_ms / 1000) + off_min * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    int y = tm.tm_year + 1900;
    // verified: Open GoPro spec 2.0 "Set Local Date Time" + SDK CmdId.SET_DATE_TIME_DST
    // @ gopro/OpenGoPro 0f963572 / gh-pages 616bfb80:
    //   cmd 0x0F, TLV len 0x0A, then 10-byte value:
    //   [year u16 BIG-ENDIAN][month][day][hour][minute][second]
    //   [utc_offset int16 BIG-ENDIAN minutes][is_dst u8]
    //   spec example "2023-01-31 03:04:05 (utc-02:00) dst:on" => 07E7 01 1F 03 04 05 FF88 01
    //   (0x0D = SET_DATE_TIME, the 7-byte no-tz variant, for older firmware.)
    uint8_t b[12] = {
        0x0F, 0x0A,
        (uint8_t)(y >> 8), (uint8_t)y,
        (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday,
        (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec,
        (uint8_t)((uint16_t)off_min >> 8), (uint8_t)off_min,
        dst ? 1 : 0,
    };
    wr(h_cmd, b, sizeof(b));
    applog_note("gopro set clock %04d-%02d-%02d %02d:%02d:%02dZ%+d",
                y, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, off_min);
    return ESP_OK;
}

static esp_err_t gp_get_datetime(void)
{
    // verified: SDK CmdId.GET_DATE_TIME_DST = 0x10 (0x0E = GET_DATE_TIME, no tz).
    // We set with the DST variant (0x0F) so we must read back tz+dst too -> 0x10.
    uint8_t b[1] = { 0x10 };
    wr(h_cmd, b, 1);
    return ESP_OK;
}

static esp_err_t gp_push_gps(const gps_fix_t *fix, int64_t utc_ms)
{
    (void)fix; (void)utc_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t gp_hilight(void)
{
    uint8_t b[1] = { 0x18 };
    wr(h_cmd, b, 1);
    return ESP_OK;
}

static esp_err_t gp_tick_1s(void)
{
    // keepalive every 3 s (spec 6.4: must be < 10 s).
    if (++keepalive_div >= 3) {
        keepalive_div = 0;
        uint8_t b[3] = { 0x5B, 0x01, 0x42 };
        wr(h_set, b, 3);
    }

    // Polling fallback -- this firmware does not appear to honour our async-push
    // registration (0x52/0x53), so nothing the user changes ON the camera
    // (record, resolution, mode) ever pushes to us. Poll instead:
    //   get-all-STATUS   (0x13, small: encoding/battery/SD/busy) every ~2 s
    //   get-all-SETTINGS  (0x12, res/fps/lens)                   every ~5 s
    uint32_t t = now_ms();
    if (t - last_status_poll_ms > 1000) {
        last_status_poll_ms = t;
        uint8_t g[1] = { 0x13 }; wr(h_qry, g, 1);
    }
    if (t - last_getall_ms > 3000) {
        last_getall_ms = t;
        uint8_t g[1] = { 0x12 }; wr(h_qry, g, 1);
    }
    (void)getall_retry;

    // local elapsed counter (spec 6.5)
    cam_state_t *s = cam_scratch();
    if (s->recording == TRIBOOL_TRUE) {
        uint32_t el = (now_ms() - rec_start_ms) / 1000;
        if (!s->rec_elapsed_s.valid || s->rec_elapsed_s.v != el) {
            s->rec_elapsed_s.valid = true;
            s->rec_elapsed_s.v = el;
            if (CB->on_state_dirty) { CB->on_state_dirty(); }
        }
    }
    return ESP_OK;
}

// ---- link bring-up --------------------------------------------
static const brand_gatt_t *gp_gatt(void)
{
    static const brand_gatt_t g = {
        .sub = { &U_CMD_RSP.u, &U_SET_RSP.u, &U_QRY_RSP.u, NULL },
        .sub_n = 3,
    };
    return &g;
}

static esp_err_t gp_link_up(bool first_pair)
{
    CB = camera_cbs();
    h_cmd = ble_central_val_handle(&U_CMD_REQ.u);
    h_set = ble_central_val_handle(&U_SET_REQ.u);
    h_qry = ble_central_val_handle(&U_QRY_REQ.u);
    vh_cmd_rsp = ble_central_val_handle(&U_CMD_RSP.u);
    vh_set_rsp = ble_central_val_handle(&U_SET_RSP.u);
    vh_qry_rsp = ble_central_val_handle(&U_QRY_RSP.u);
    if (!h_cmd || !h_qry) {
        ESP_LOGE(TAG, "required GoPro characteristics missing");
        return ESP_FAIL;
    }
    gopro_reasm_reset(&rz_cmd);
    gopro_reasm_reset(&rz_set);
    gopro_reasm_reset(&rz_qry);
    keepalive_div = 0;
    getall_retry = 0;
    last_getall_ms = now_ms();
    last_status_poll_ms = now_ms();

    gp_pull_all();

    applog_note("gopro link up%s", first_pair ? " (first pair)" : "");
    if (first_pair && CB->on_paired) {
        const cam_state_t *s = cam_scratch();
        CB->on_paired(s->addr, s->name[0] ? s->name : "GoPro", 0);
    }
    // GoPro sync is fire-and-forget writes; the link is usable immediately and
    // status/setting pushes will fill cam_state within ~1 s.
    if (CB->on_link_state) { CB->on_link_state(CAM_LINK_READY); }
    return ESP_OK;
}

static void gp_link_down(void)
{
    h_cmd = h_set = h_qry = 0;
    want_record_ack = false;
}

// Register for async updates (one ID per request -- the documented form) and
// pull every value now. Shared by link-up and the portal "refresh" button.
static void gp_pull_all(void)
{
    static const uint8_t reg_st[]  = { ST_ENCODING, ST_BUSY, ST_HOT, ST_BATT_PCT,
                                       ST_SD_REMAIN_KB, ST_REMAIN_VIDEO_S,
                                       ST_SD_STATUS, ST_ACTIVE_PRESET };
    static const uint8_t reg_set[] = { SET_RESOLUTION, SET_FPS, SET_LENS };
    for (unsigned i = 0; i < sizeof(reg_st); i++)  { uint8_t b[2] = { 0x53, reg_st[i] };  wr(h_qry, b, 2); }
    for (unsigned i = 0; i < sizeof(reg_set); i++) { uint8_t b[2] = { 0x52, reg_set[i] }; wr(h_qry, b, 2); }
    uint8_t g13[1] = { 0x13 }; wr(h_qry, g13, 1);
    uint8_t g12[1] = { 0x12 }; wr(h_qry, g12, 1);
}

static esp_err_t gp_refresh(void)
{
    if (!h_qry) { return ESP_ERR_INVALID_STATE; }
    applog_note("gopro: manual refresh");
    gopro_reasm_reset(&rz_qry);
    last_getall_ms = last_status_poll_ms = now_ms();
    gp_pull_all();
    return ESP_OK;
}

// ---- notification parsing -----------------------------------
// Big-endian integer. If the field is wider than 32 bits (newer GoPro fw sends
// status 54 / 35 as 8 bytes), keep the LEAST-significant 32 bits -- i.e. the
// trailing bytes -- not the leading zeros (that made SD read 0.0 GB).
static uint32_t be(const uint8_t *p, uint8_t n)
{
    if (n > 4) { p += (n - 4); n = 4; }
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) { v = (v << 8) | p[i]; }
    return v;
}

// Mutates the scratch in place; the caller (walk_tuples) commits once at the end.
static void apply_status(cam_state_t *s, uint8_t id, const uint8_t *v, uint8_t n)
{
    // Temporary: log the candidate mode/preset statuses (raw) so the right one
    // can be identified from a capture while switching Photo<->Video.
    switch (id) {
    case 19: case 89: case 90: case 93: case 96: case 97: case 98: case 99:
        ESP_LOGI(TAG, "status %3u len %u = %02x %02x %02x %02x", id, n,
                 n > 0 ? v[0] : 0, n > 1 ? v[1] : 0, n > 2 ? v[2] : 0, n > 3 ? v[3] : 0);
        break;
    default: break;
    }
    switch (id) {
    case ST_ENCODING: {
        tribool_t was = s->recording;
        s->recording = v[0] ? TRIBOOL_TRUE : TRIBOOL_FALSE;
        if (was != s->recording) {
            ESP_LOGI(TAG, "encoding -> %s", v[0] ? "REC" : "idle");
        }
        if (s->recording == TRIBOOL_TRUE && was != TRIBOOL_TRUE) {
            rec_start_ms = now_ms();
            s->rec_elapsed_s.valid = true; s->rec_elapsed_s.v = 0;
        } else if (s->recording == TRIBOOL_FALSE) {
            s->rec_elapsed_s.valid = false;
        }
        if (was != s->recording && CB->on_ack) {
            // treat a state change matching our request as the ack
            if (want_record_ack && tribool_is(s->recording, want_record_target)) {
                want_record_ack = false;
                CB->on_ack(want_record_target ? CAM_OP_RECORD_START : CAM_OP_RECORD_STOP, true);
            }
        }
        break;
    }
    case ST_BUSY: s->busy = v[0] ? TRIBOOL_TRUE : TRIBOOL_FALSE; break;
    case ST_HOT:  s->hot = v[0] != 0; break;
    case ST_BATT_PCT: s->battery_pct.valid = true; s->battery_pct.v = v[0]; break;
    case ST_SD_REMAIN_KB:
        s->sd_remaining_kb.valid = true; s->sd_remaining_kb.v = be(v, n);
        ESP_LOGI(TAG, "SD remaining: %lu KB (~%lu MB), field len %u",
                 (unsigned long)s->sd_remaining_kb.v,
                 (unsigned long)(s->sd_remaining_kb.v / 1024), n);
        break;
    case ST_REMAIN_VIDEO_S:
        s->rec_remaining_s.valid = true; s->rec_remaining_s.v = be(v, n); break;
    case ST_SD_STATUS:
        // verified: Open GoPro spec 2.0 PrimaryStorage enum @ statuses.py 0f963572:
        //   -1(0xFF) UNKNOWN, 0 OK, 1 SD_CARD_FULL, 2 SD_CARD_REMOVED,
        //   3 SD_CARD_FORMAT_ERROR, 4 SD_CARD_BUSY, 8 SD_CARD_SWAPPED.
        // 4 (BUSY) is a present card, NOT missing -- was wrongly flagged before.
        // FIXME(perf-coherence): decide if 3 (FORMAT_ERROR) should block recording.
        s->sd_missing = (v[0] == 2);
        s->sd_full    = (v[0] == 1);
        break;
    case ST_ACTIVE_PRESET: {
        // Preset ID high bits = group: 0x0000xxxx video, 0x0001xxxx photo,
        // 0x0002xxxx timelapse (VERIFY.md GOPRO-STATUS-89). GoPro "mode" only
        // exists as the preset group -- switching to Photo does not change the
        // video-resolution setting, so blank res/fps when not in a video mode.
        uint32_t pid = be(v, n);
        uint8_t grp = (pid >> 16) & 0xFF;
        if (s->mode.valid && s->mode.v != grp) {
            getall_retry = 0;           // re-poll settings for the new mode
            last_getall_ms = now_ms();
        }
        s->mode.valid = true; s->mode.v = grp;
        const char *mn = grp == 0 ? "VID" : grp == 1 ? "PHOTO" : grp == 2 ? "TLAPSE" : "";
        val_str_set(&s->mode_name, mn);
        if (!*mn) { s->mode_name.valid = false; }
        if (grp != 0) {
            s->resolution.valid = false;
            s->framerate.valid = false;
            s->aspect.valid = false;
        }
        ESP_LOGI(TAG, "active preset 0x%08lx -> %s", (unsigned long)pid, mn);
        break;
    }
    default: break;
    }
}

static void apply_setting(cam_state_t *s, uint8_t id, const uint8_t *v, uint8_t n)
{
    (void)n;
    switch (id) {
    case SET_RESOLUTION:
        val_str_set(&s->resolution, gopro_resolution_str(v[0]));
        if (!*s->resolution.v) { s->resolution.valid = false; }
        val_str_set(&s->aspect, gopro_aspect_str(v[0]));
        if (!*s->aspect.v) { s->aspect.valid = false; }
        break;
    case SET_FPS:
        val_str_set(&s->framerate, gopro_fps_str(v[0]));
        if (!*s->framerate.v) { s->framerate.valid = false; }
        break;
    case SET_LENS:
        val_str_set(&s->lens, gopro_lens_str(v[0]));
        if (!*s->lens.v) { s->lens.valid = false; }
        break;
    default:
        break;
    }
}

static void walk_tuples(const uint8_t *p, uint16_t len, bool is_status)
{
    cam_state_t *s = cam_scratch();
    uint16_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = p[i++];
        uint8_t n  = p[i++];
        if (i + n > len) { break; }
        if (n) {
            if (is_status) { apply_status(s, id, p + i, n); }
            else           { apply_setting(s, id, p + i, n); }
        }
        i += n;
    }
    if (CB->on_state_dirty) { CB->on_state_dirty(); }   // commits + wakes osd_task
}

static void parse_cmd_rsp(const uint8_t *p, uint16_t len)
{
    if (len < 2) { return; }
    uint8_t cmd_id = p[0];
    uint8_t status = p[1];
    if (cmd_id == 0x10 && status == 0 && len >= 12) {
        // GET_DATE_TIME_DST (0x10) response.
        // FIXME(protocol-verifier): UNRESOLVED -- the exact response framing is
        // not documented in a machine-readable form. The Open GoPro Python SDK
        // parser (DateTimeByteParserBuilder.parse) skips ONE leading byte
        // (data[1:]) before the year, implying a payload length/count byte after
        // [cmd_id][status]. If so, every field below is off by one and 'off'
        // reads garbage. Needs a real HERO 11/12/13 BLE capture to confirm.
        // Assumed here (no length byte): [id][st][yr hi][yr lo][mon][day][hr][min][sec][off hi][off lo][dst]
        int y = (p[2] << 8) | p[3];
        int mon = p[4], day = p[5], hh = p[6], mm = p[7], ss = p[8];
        int16_t off = (int16_t)((p[9] << 8) | p[10]);
        int64_t local_epoch = epoch_from_civil_utc(y, mon, day, hh, mm, ss);
        int64_t utc_ms = (local_epoch - off * 60) * 1000;
        cam_state_t *s = cam_scratch();
        s->cam_utc_ms.valid = true;
        s->cam_utc_ms.v = utc_ms;
        s->cam_clock_read_uptime_ms = now_ms();
        if (CB->on_state_dirty) { CB->on_state_dirty(); }
        ESP_LOGI(TAG, "gopro clock read: %04d-%02d-%02d %02d:%02d:%02d off%+d",
                 y, mon, day, hh, mm, ss, off);
    } else if (cmd_id == 0x01) {
        if (CB->on_ack) {
            CB->on_ack(want_record_target ? CAM_OP_RECORD_START : CAM_OP_RECORD_STOP, status == 0);
        }
    } else if (cmd_id == 0x40 || cmd_id == 0x3E) {
        if (CB->on_ack) { CB->on_ack(CAM_OP_SELECT_PRESET, status == 0); }
    }
}

static void gp_on_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    const uint8_t *pl; uint16_t pll;
    if (handle == vh_cmd_rsp) {
        if (gopro_reasm_feed(&rz_cmd, data, len, &pl, &pll)) { parse_cmd_rsp(pl, pll); }
    } else if (handle == vh_set_rsp) {
        // Response to our keepalive / any setting write: [id][status]. Nothing
        // to consume (we never write settings we care about).
        (void)gopro_reasm_feed(&rz_set, data, len, &pl, &pll);
    } else if (handle == vh_qry_rsp) {
        char hx[52]; int hn = 0;
        for (int i = 0; i < len && i < 16; i++) hn += snprintf(hx + hn, sizeof(hx) - hn, "%02x ", data[i]);
        ESP_LOGI(TAG, "QRY raw len=%u: %s", len, hx);
        if (gopro_reasm_feed(&rz_qry, data, len, &pl, &pll) && pll >= 2) {
            uint8_t qid = pl[0];
            // Every TLV query response -- get-all (0x12/0x13), register echo
            // (0x52/0x53) AND async push (0x92/0x93) -- is [id][status][tlvs...]
            // (verified: OpenGoPro tutorial_modules TlvResponse.parse @ 0f963572).
            // Settings live under 0x12/0x52/0x92; statuses under 0x13/0x53/0x93.
            bool is_setting = (qid == 0x12 || qid == 0x52 || qid == 0x92);
            ESP_LOGI(TAG, "query id=%02x st=%02x len=%u (%s)",
                     qid, pl[1], pll, is_setting ? "setting" : "status");
            if (pll > 2) { walk_tuples(pl + 2, pll - 2, !is_setting); }
        }
    } else {
        ESP_LOGD(TAG, "notify on unknown handle %u len %u", handle, len);
    }
}

// ---- adv match (spec 6.1) ----------------------------------
static bool gp_adv_match(const ble_adv_t *adv)
{
    if (adv->svc_uuid16 == GP_SVC_UUID16) { return true; }
    if (strncmp(adv->name, "GoPro ", 6) == 0) { return true; }
    return false;
}

const camera_driver_t *gopro_driver(void)
{
    static const camera_driver_t d = {
        .name = "gopro",
        .adv_match = gp_adv_match,
        .gatt = gp_gatt,
        .link_up = gp_link_up,
        .link_down = gp_link_down,
        .record = gp_record,
        .select_preset = gp_select_preset,
        .set_datetime = gp_set_datetime,
        .get_datetime = gp_get_datetime,
        .push_gps = gp_push_gps,
        .hilight = gp_hilight,
        .tick_1s = gp_tick_1s,
        .refresh = gp_refresh,
        .on_notify = gp_on_notify,
    };
    return &d;
}
