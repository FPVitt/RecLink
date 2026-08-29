// dji_driver.c -- DJI DUML BLE driver (spec 7). cam_task context only.
// Fire-and-forget writes; state arrives via the 1 Hz status push 0x1D/0x02.
#include "camera_dji.h"
#include "dji_duml.h"
#include "dji_enums.h"

#include <string.h>
#include <time.h>

#include "cfg.h"
#include "cam_state.h"
#include "ble_central.h"
#include "monoclock.h"
#include "applog.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "host/ble_uuid.h"

static const char *TAG = "dji";

// ---- GATT (spec 7.1) ---------------------------------------------
static const ble_uuid16_t U_SVC   = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t U_NOTIFY = BLE_UUID16_INIT(0xFFF4);
static const ble_uuid16_t U_WRITE  = BLE_UUID16_INIT(0xFFF5);

// ---- CmdSet/CmdID (spec 7.4) -----------------------------------
#define CS_COMMON 0x00
#define CS_CAMERA 0x1D
#define ID_CONNECT      0x19   // CS_COMMON
#define ID_GPS_PUSH     0x17   // CS_COMMON
#define ID_KEY_REPORT   0x11   // CS_COMMON
#define ID_REC          0x03   // CS_CAMERA
#define ID_MODE         0x04   // CS_CAMERA
#define ID_SUBSCRIBE    0x05   // CS_CAMERA
#define ID_STATUS_PUSH  0x02   // CS_CAMERA (camera -> us)

static const camera_cbs_t *CB;
static uint16_t h_write;
static uint16_t vh_notify;
static uint16_t s_seq;
static uint32_t s_device_id;
static bool     s_first_pair;
static bool     s_handshaked;
static duml_reasm_t rz;

// ---- helpers ---------------------------------------------------
static void tx(uint8_t cmd_set, uint8_t cmd_id, const uint8_t *pl, uint16_t n)
{
    if (!h_write) { return; }
    uint8_t frame[DUML_MAX_FRAME];
    size_t len = duml_build(frame, sizeof(frame), 0x00, s_seq++, cmd_set, cmd_id, pl, n);
    if (len) { ble_central_write(h_write, frame, len, false); }
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get_u32(const uint8_t *p) { return p[0] | (p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t get_u16(const uint8_t *p) { return p[0] | (p[1]<<8); }

// ---- handshake (spec 7.3) -----------------------------------
static void send_connect_request(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);

    uint8_t p[4 + 1 + 16 + 4 + 1 + 1 + 2];
    memset(p, 0, sizeof(p));
    size_t i = 0;
    put_u32(&p[i], s_device_id); i += 4;
    p[i++] = 6;                                  // mac_addr_len
    memcpy(&p[i], mac, 6); i += 16;              // mac_addr int8[16]
    put_u32(&p[i], 0x00000001); i += 4;          // fw_version (ours)
    p[i++] = 0;                                  // conidx
    p[i++] = s_first_pair ? 1 : 0;               // verify_mode (VERIFY.md DJI-HANDSHAKE)
    p[i++] = 0; p[i++] = 0;                      // verify_data u16
    tx(CS_COMMON, ID_CONNECT, p, sizeof(p));
    applog_note("dji connect req id=%08lx verify=%d", (unsigned long)s_device_id, s_first_pair);
}

static void send_subscribe(void)
{
    // push_mode 2 = periodic, push_freq 10 = 1 Hz (units 0.1 Hz), reserved[4]
    uint8_t p[6] = { 2, 10, 0, 0, 0, 0 };
    tx(CS_CAMERA, ID_SUBSCRIBE, p, sizeof(p));
}

// ---- ops ---------------------------------------------------
static esp_err_t dji_record(bool start)
{
    uint8_t p[9]; memset(p, 0, sizeof(p));
    put_u32(p, s_device_id);
    p[4] = start ? 0 : 1;             // record_ctrl: 0 start, 1 stop
    tx(CS_CAMERA, ID_REC, p, sizeof(p));
    return ESP_OK;
}

static esp_err_t dji_select_preset(uint8_t slot)
{
    if (slot > 2) { return ESP_ERR_INVALID_ARG; }
    const cfg_t *c = cfg_get();
    uint8_t mode = (uint8_t)c->preset.bands[slot].id;   // DJI: band id == mode value
    uint8_t p[9]; memset(p, 0, sizeof(p));
    put_u32(p, s_device_id);
    p[4] = mode;
    tx(CS_CAMERA, ID_MODE, p, sizeof(p));
    return ESP_OK;
}

// spec 7.4 VERIFY.md DJI-DATETIME: year_month_day / hour_minute_second encoding
// and UTC-vs-local. Using the common DUML convention: decimal-packed, UTC.
static void fill_datetime(uint8_t *p8, int64_t utc_ms)
{
    time_t t = (time_t)(utc_ms / 1000);
    struct tm tm; gmtime_r(&t, &tm);
    int32_t ymd = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    int32_t hms = tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec;
    put_u32(&p8[0], (uint32_t)ymd);
    put_u32(&p8[4], (uint32_t)hms);
}

static esp_err_t dji_push_gps(const gps_fix_t *fix, int64_t utc_ms)
{
    uint8_t p[8 + 4*4 + 3*4 + 4*4];
    memset(p, 0, sizeof(p));
    fill_datetime(&p[0], utc_ms);
    size_t i = 8;
    put_u32(&p[i], (uint32_t)(fix ? fix->lon_1e7 : 0)); i += 4;
    put_u32(&p[i], (uint32_t)(fix ? fix->lat_1e7 : 0)); i += 4;
    put_u32(&p[i], (uint32_t)(fix ? fix->alt_m * 1000 : 0)); i += 4;   // height mm
    i += 12;                                                          // vN/vE/vD f32 = 0
    put_u32(&p[i], 500); i += 4;                                       // vAcc
    put_u32(&p[i], 500); i += 4;                                       // hAcc
    put_u32(&p[i], 500); i += 4;                                       // sAcc
    put_u32(&p[i], fix ? fix->sats : 0); i += 4;
    tx(CS_COMMON, ID_GPS_PUSH, p, i);                                  // no response
    return ESP_OK;
}

static esp_err_t dji_set_datetime(int64_t utc_ms, int16_t off_min, bool dst)
{
    (void)off_min; (void)dst;
    // DJI blind sync: no dedicated clock cmd exposed -> ride the GPS push time
    // fields (spec 7.4). Send a position-less push carrying only the time.
    return dji_push_gps(NULL, utc_ms);
}

static esp_err_t dji_get_datetime(void)
{
    return ESP_ERR_NOT_SUPPORTED;   // not readable over DUML status (spec 7.4)
}

static esp_err_t dji_hilight(void)
{
    // maps to QS key (spec 7.4 / 16.D)
    uint8_t p[4] = { 2 /*QS*/, 0, 0, 0 };
    tx(CS_COMMON, ID_KEY_REPORT, p, sizeof(p));
    return ESP_OK;
}

static esp_err_t dji_tick_1s(void)
{
    // DUML periodic push is the keepalive; nothing to send. Re-arm subscription
    // if we somehow stopped hearing pushes is handled by cam_task RX timeout.
    return ESP_OK;
}

// ---- link bring-up --------------------------------------
static const brand_gatt_t *dji_gatt(void)
{
    static const brand_gatt_t g = { .sub = { &U_NOTIFY.u, NULL, NULL, NULL }, .sub_n = 1 };
    return &g;
}

static esp_err_t dji_link_up(bool first_pair)
{
    CB = camera_cbs();
    h_write = ble_central_val_handle(&U_WRITE.u);
    vh_notify = ble_central_val_handle(&U_NOTIFY.u);
    if (!h_write || !vh_notify) {
        ESP_LOGE(TAG, "DUML characteristics missing");
        return ESP_FAIL;
    }
    duml_reasm_reset(&rz);
    s_first_pair = first_pair;
    s_handshaked = false;

    const cfg_t *c = cfg_get();
    s_device_id = c->camera.dji_device_id;
    if (s_device_id == 0) {
        s_device_id = esp_random() | 0x01000000u;   // non-zero, persisted on pair
    }
    send_connect_request();
    return ESP_OK;      // stays SYNCING until handshake response
}

static void dji_link_down(void)
{
    h_write = vh_notify = 0;
    s_handshaked = false;
}

// ---- status push parsing (spec 7.5) --------------------
// VERIFY.md DJI-STATUS-PUSH: field order/packing of 0x1D/0x02.
static void parse_status_push(const uint8_t *p, uint16_t n)
{
    if (n < 17) { return; }
    cam_state_t s; cam_state_get(&s);

    uint8_t  camera_mode  = p[0];
    uint8_t  camera_status = p[1];
    uint8_t  res_idx      = p[2];
    uint8_t  fps_idx      = p[3];
    /* uint8_t eis = p[4]; */
    uint16_t record_time  = get_u16(&p[5]);
    uint32_t remain_mb    = get_u32(&p[7]);
    uint32_t remain_time  = get_u32(&p[11]);
    uint8_t  batt_pct     = p[15];
    /* uint8_t power_mode = p[16]; */

    s.mode.valid = true; s.mode.v = camera_mode;
    val_str_set(&s.mode_name, dji_mode_str(camera_mode));
    if (!*s.mode_name.v) { s.mode_name.valid = false; }

    // recording: camera_status 3 = photo/recording, video mode -> recording.
    bool is_video = (camera_mode == 0 || camera_mode == 2 || camera_mode == 6);
    tribool_t rec = (camera_status == 3 && is_video) ? TRIBOOL_TRUE :
                    (camera_status == 5 ? TRIBOOL_TRUE : TRIBOOL_FALSE);
    if (rec != s.recording && CB->on_ack) {
        // no explicit ack on 0x1D/0x03 resp handling here; treat push as truth
    }
    s.recording = rec;

    s.rec_elapsed_s.valid = true; s.rec_elapsed_s.v = record_time;
    s.rec_remaining_s.valid = true; s.rec_remaining_s.v = remain_time;
    s.sd_remaining_kb.valid = true; s.sd_remaining_kb.v = remain_mb * 1024;   // MB -> KB
    s.sd_missing = (remain_mb == 0);
    s.battery_pct.valid = true; s.battery_pct.v = batt_pct;

    val_str_set(&s.resolution, dji_resolution_str(res_idx));
    if (!*s.resolution.v) { s.resolution.valid = false; }
    val_str_set(&s.framerate, dji_fps_str(fps_idx));
    if (!*s.framerate.v) { s.framerate.valid = false; }

    s.busy = TRIBOOL_FALSE;   // DJI has no explicit busy; mode switch latency handled by ctrl dwell

    cam_state_publish(&s);
    if (CB->on_state_dirty) { CB->on_state_dirty(); }
}

static void handle_msg(const duml_msg_t *m)
{
    if (m->cmd_set == CS_COMMON && m->cmd_id == ID_CONNECT) {
        // resp: device_id u32, ret_code u8
        uint8_t ret = (m->payload_len >= 5) ? m->payload[4] : 0xFF;
        ESP_LOGI(TAG, "connect resp ret=%u", ret);
        if (ret == 0) {
            s_handshaked = true;
            send_subscribe();
            if (s_first_pair && CB->on_paired) {
                cam_state_t s; cam_state_get(&s);
                CB->on_paired(s.addr, s.name[0] ? s.name : "DJI Osmo", s_device_id);
                s_first_pair = false;
            }
            if (CB->on_link_state) { CB->on_link_state(CAM_LINK_READY); }
        } else {
            applog_note("dji handshake rejected ret=%u", ret);
            // stay SYNCING -> cam_task CONNECT_TIMEOUT_MS will rescan
        }
    } else if (m->cmd_set == CS_CAMERA && m->cmd_id == ID_STATUS_PUSH) {
        parse_status_push(m->payload, m->payload_len);
    } else if (m->cmd_set == CS_CAMERA && (m->cmd_id == ID_REC || m->cmd_id == ID_MODE)) {
        uint8_t ret = m->payload_len ? m->payload[0] : 0xFF;
        if (CB->on_ack) {
            CB->on_ack(m->cmd_id == ID_REC ? CAM_OP_RECORD_START : CAM_OP_SELECT_PRESET, ret == 0);
        }
    }
}

static void dji_on_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    if (handle != vh_notify) { return; }
    duml_msg_t m;
    if (duml_reasm_feed(&rz, data, len, &m)) { handle_msg(&m); }
    while (duml_reasm_next(&rz, &m)) { handle_msg(&m); }
}

// ---- adv match (spec 7.1) ---------------------------------
static bool dji_adv_match(const ble_adv_t *adv)
{
    // manufacturer-data bytes [0]=0xAA [1]=0x08 [4]=0xFA
    if (adv->mfg_len >= 5 && adv->mfg[0] == 0xAA && adv->mfg[1] == 0x08 && adv->mfg[4] == 0xFA) {
        return true;
    }
    return false;
}

const camera_driver_t *dji_driver(void)
{
    static const camera_driver_t d = {
        .name = "dji",
        .adv_match = dji_adv_match,
        .gatt = dji_gatt,
        .link_up = dji_link_up,
        .link_down = dji_link_down,
        .record = dji_record,
        .select_preset = dji_select_preset,
        .set_datetime = dji_set_datetime,
        .get_datetime = dji_get_datetime,
        .push_gps = dji_push_gps,
        .hilight = dji_hilight,
        .tick_1s = dji_tick_1s,
        .on_notify = dji_on_notify,
    };
    return &d;
}
