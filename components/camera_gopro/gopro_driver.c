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

// ---- status / setting IDs (spec 16.B; VERIFY.md GOPRO-STATUS-*) ----------
enum { ST_BATT_BARS = 2, ST_HOT = 6, ST_BUSY = 8, ST_ENCODING = 10,
       ST_SD_STATUS = 33, ST_REMAIN_VIDEO_S = 35, ST_SD_REMAIN_KB = 54, ST_BATT_PCT = 70 };
enum { SET_RESOLUTION = 2, SET_FPS = 3, SET_LENS = 121 };

// ---- driver state ---------------------------------------------------
static const camera_cbs_t *CB;
static uint16_t h_cmd, h_set, h_qry;      // write value handles
static uint16_t vh_cmd_rsp, vh_set_rsp, vh_qry_rsp;
static gopro_reasm_t rz_cmd, rz_set, rz_qry;
static uint8_t  keepalive_div;
static uint32_t rec_start_ms;
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
    // spec 6.4: cmd 0x0F -- [yr u16 BE][mon][day][hr][min][sec][off i16 min][dst]
    // VERIFY.md GOPRO-CMD-DATETIME: 0x0F vs 0x0D per firmware.
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
    uint8_t b[1] = { 0x0E };
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
    // local elapsed counter (spec 6.5)
    cam_state_t s; cam_state_get(&s);
    if (s.recording == TRIBOOL_TRUE) {
        uint32_t el = (now_ms() - rec_start_ms) / 1000;
        if (!s.rec_elapsed_s.valid || s.rec_elapsed_s.v != el) {
            s.rec_elapsed_s.valid = true;
            s.rec_elapsed_s.v = el;
            cam_state_publish(&s);
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

    // Register async status + setting updates, then pull initial values.
    uint8_t regs[] = { 0x53, ST_ENCODING, ST_BUSY, ST_HOT, ST_BATT_PCT,
                       ST_SD_REMAIN_KB, ST_REMAIN_VIDEO_S, ST_SD_STATUS };
    wr(h_qry, regs, sizeof(regs));
    uint8_t regset[] = { 0x52, SET_RESOLUTION, SET_FPS, SET_LENS };
    wr(h_qry, regset, sizeof(regset));
    uint8_t getall_st[] = { 0x13 };
    wr(h_qry, getall_st, 1);
    uint8_t getall_set[] = { 0x12 };
    wr(h_qry, getall_set, 1);

    applog_note("gopro link up%s", first_pair ? " (first pair)" : "");
    if (first_pair && CB->on_paired) {
        cam_state_t s; cam_state_get(&s);
        CB->on_paired(s.addr, s.name[0] ? s.name : "GoPro", 0);
    }
    return ESP_OK;
}

static void gp_link_down(void)
{
    h_cmd = h_set = h_qry = 0;
    want_record_ack = false;
}

// ---- notification parsing -----------------------------------
static uint32_t be(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < n && i < 4; i++) { v = (v << 8) | p[i]; }
    return v;
}

static void apply_status(cam_state_t *s, uint8_t id, const uint8_t *v, uint8_t n)
{
    bool dirty = true;
    switch (id) {
    case ST_ENCODING: {
        tribool_t was = s->recording;
        s->recording = v[0] ? TRIBOOL_TRUE : TRIBOOL_FALSE;
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
        s->sd_remaining_kb.valid = true; s->sd_remaining_kb.v = be(v, n); break;
    case ST_REMAIN_VIDEO_S:
        s->rec_remaining_s.valid = true; s->rec_remaining_s.v = be(v, n); break;
    case ST_SD_STATUS:
        // VERIFY.md GOPRO-STATUS-33: 0 OK, 1 full, 2 removed, 3 format error...
        s->sd_missing = (v[0] == 2 || v[0] == 4);
        s->sd_full    = (v[0] == 1);
        break;
    default: dirty = false; break;
    }
    if (dirty && CB->on_state_dirty) { CB->on_state_dirty(); }
}

static void apply_setting(cam_state_t *s, uint8_t id, const uint8_t *v, uint8_t n)
{
    (void)n;
    switch (id) {
    case SET_RESOLUTION: val_str_set(&s->resolution, gopro_resolution_str(v[0]));
                         if (!*s->resolution.v) s->resolution.valid = false; break;
    case SET_FPS: val_str_set(&s->framerate, gopro_fps_str(v[0]));
                  if (!*s->framerate.v) s->framerate.valid = false; break;
    case SET_LENS: val_str_set(&s->lens, gopro_lens_str(v[0]));
                   if (!*s->lens.v) s->lens.valid = false; break;
    default: return;
    }
    if (CB->on_state_dirty) { CB->on_state_dirty(); }
}

static void walk_tuples(const uint8_t *p, uint16_t len, bool is_status)
{
    cam_state_t s; cam_state_get(&s);
    uint16_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = p[i++];
        uint8_t n  = p[i++];
        if (i + n > len) { break; }
        if (n) {
            if (is_status) { apply_status(&s, id, p + i, n); }
            else           { apply_setting(&s, id, p + i, n); }
        }
        i += n;
    }
    cam_state_publish(&s);
}

static void parse_cmd_rsp(const uint8_t *p, uint16_t len)
{
    if (len < 2) { return; }
    uint8_t cmd_id = p[0];
    uint8_t status = p[1];
    if (cmd_id == 0x0E && status == 0 && len >= 12) {
        // get datetime: [0x0E][st][yr hi][yr lo][mon][day][hr][min][sec][off hi][off lo][dst]
        int y = (p[2] << 8) | p[3];
        int mon = p[4], day = p[5], hh = p[6], mm = p[7], ss = p[8];
        int16_t off = (int16_t)((p[9] << 8) | p[10]);
        int64_t local_epoch = epoch_from_civil_utc(y, mon, day, hh, mm, ss);
        int64_t utc_ms = (local_epoch - off * 60) * 1000;
        cam_state_t s; cam_state_get(&s);
        s.cam_utc_ms.valid = true;
        s.cam_utc_ms.v = utc_ms;
        s.cam_clock_read_uptime_ms = now_ms();
        cam_state_publish(&s);
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
        if (gopro_reasm_feed(&rz_set, data, len, &pl, &pll) && pll >= 2) {
            walk_tuples(pl + 2, pll - 2, false);
        }
    } else if (handle == vh_qry_rsp) {
        if (gopro_reasm_feed(&rz_qry, data, len, &pl, &pll) && pll >= 2) {
            uint8_t qid = pl[0];
            // 0x12 get-all-settings / 0x52 setting push -> settings; else statuses
            bool is_status = !(qid == 0x12 || qid == 0x52 || qid == 0x92);
            walk_tuples(pl + 2, pll - 2, is_status);
        }
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
        .on_notify = gp_on_notify,
    };
    return &d;
}
