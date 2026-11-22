// dji_driver.c -- DJI DUML BLE driver (spec 7). cam_task context only.
// Fire-and-forget writes; state arrives via the 2 Hz status push 0x1D/0x02.
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
// verified: dji-sdk/Osmo-GPS-Controller-Demo ble/ble.c:81-83
// @ 92fe23e5a749f189593f980a26a105c3bb66aa1c (2026-08-29):
//   primary service 0xFFF0, notify char 0xFFF4, write char 0xFFF5.
// (The service UUID is not needed here -- the generic cam_task discovery keys
// off the characteristic UUIDs below -- so it is recorded only in this comment.)
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
#define ID_STATUS_PUSH2 0x06   // CS_CAMERA: mode / param text, sent by the Action 6 on every mode change

// connection_request_command_frame length (spec 7.3): device_id[4] mac_len[1]
// mac[16] fw[4] conidx[1] verify_mode[1] verify_data[2] reserved[4]. @ 92fe23e5
#define DJI_CONN_REQ_LEN 33

static const camera_cbs_t *CB;
static uint16_t h_write;
static uint16_t vh_notify;
static uint16_t s_seq;
static uint32_t s_device_id;
static bool     s_first_pair;
static bool     s_handshaked;
static duml_reasm_t rz, rz5;      // reassembly per notify characteristic (FFF4, FFF5)
static uint8_t s_dbg_n;           // diagnostic: notifications logged on this link
static uint32_t s_last_rx_ms;     // last notification, to shed a stale half frame
static char    s_txt_res[8], s_txt_fps[6];
static bool    s_txt_valid;
static uint8_t s_txt_mode = 0xFF;             // camera_mode (1D/02) the text belongs to; 0xFF = not yet known
static bool    s_asleep;                      // power_mode == 3 seen; log once per link
static char    s_model[24];                   // from the camera-reported device id

// ---- helpers ---------------------------------------------------
// CmdType (spec 7.2 / demo docs/protocol.md): bit[5]=0 command frame,
// bits[4:0]=response type. verified: dji-sdk/Osmo-GPS-Controller-Demo
// logic/command_logic.c + connect_logic.c + status_logic.c
// @ 92fe23e5a749f189593f980a26a105c3bb66aa1c (2026-08-29):
//   0x00 CMD_NO_RESPONSE  (GPS push 0x00/0x17, status subscribe 0x1D/0x05)
//   0x01 CMD_RESPONSE_OR_NOT (record 0x1D/0x03, mode 0x1D/0x04, key 0x00/0x11)
//   0x02 CMD_WAIT_RESULT  (connection request 0x00/0x19)
#define DJI_CMD_NO_RESPONSE   0x00
#define DJI_CMD_RESPONSE_OPT  0x01
#define DJI_CMD_WAIT_RESULT   0x02
#define DJI_ACK_NO_RESPONSE   0x20   // response frame, no reply expected

// Sends a frame and returns the seq it carried (the camera echoes it in its reply).
static uint16_t tx(uint8_t cmd_type, uint8_t cmd_set, uint8_t cmd_id, const uint8_t *pl, uint16_t n)
{
    uint16_t seq = s_seq++;
    if (!h_write) { return seq; }
    uint8_t frame[DUML_MAX_FRAME];
    size_t len = duml_build(frame, sizeof(frame), cmd_type, seq, cmd_set, cmd_id, pl, n);
    if (len) { ble_central_write(h_write, frame, len, false); }
    return seq;
}

// ---- reply matching ------------------------------------------------
// An Action 6 answers a 1D/04 mode switch with a frame whose cmd id is 1D/03 (the record-control
// reply id), but always with the SAME seq as our request. So replies are matched by seq against the
// commands we actually sent, never by cmd id. A small ring covers retries still in flight.
typedef enum { PEND_NONE = 0, PEND_REC_START, PEND_REC_STOP, PEND_PRESET } pend_op_t;
#define PEND_N 6
static struct { uint16_t seq; pend_op_t op; } s_pend[PEND_N];
static uint8_t s_pend_i;
static void pend_add(uint16_t seq, pend_op_t op)
{
    s_pend[s_pend_i].seq = seq; s_pend[s_pend_i].op = op;
    s_pend_i = (uint8_t)((s_pend_i + 1) % PEND_N);
}
static pend_op_t pend_take(uint16_t seq)
{
    for (int i = 0; i < PEND_N; i++) {
        if (s_pend[i].op != PEND_NONE && s_pend[i].seq == seq) {
            pend_op_t op = s_pend[i].op; s_pend[i].op = PEND_NONE; return op;
        }
    }
    return PEND_NONE;
}

// ACK a camera-initiated command frame -- must echo the camera's SEQ.
static void tx_ack(uint16_t seq, uint8_t cmd_set, uint8_t cmd_id, const uint8_t *pl, uint16_t n)
{
    if (!h_write) { return; }
    uint8_t frame[DUML_MAX_FRAME];
    size_t len = duml_build(frame, sizeof(frame), DJI_ACK_NO_RESPONSE, seq, cmd_set, cmd_id, pl, n);
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

    // verified: connection_request_command_frame @ 92fe23e5 (2026-08-29)
    //   device_id u32 | mac_addr_len u8 | mac_addr int8[16] | fw_version u32 |
    //   conidx u8 | verify_mode u8 | verify_data u16 | reserved u8[4]  => 33 bytes
    //   (docs/protocol_data_segment.md "Connection Request (0019)",
    //    test/connect_cmd_frame_builder/connect_cmd_frame.txt frame len 0x33).
    //   verify_mode: 1 = first pairing (camera shows code popup), 0 = already
    //   paired (camera decides from saved history). fw_version 0 => no FW push.
    uint8_t p[4 + 1 + 16 + 4 + 1 + 1 + 2 + 4];   // 33
    memset(p, 0, sizeof(p));
    size_t i = 0;
    put_u32(&p[i], s_device_id); i += 4;
    p[i++] = 6;                                  // mac_addr_len
    memcpy(&p[i], mac, 6); i += 16;              // mac_addr int8[16]
    put_u32(&p[i], 0x00000000); i += 4;          // fw_version 0 (no FW push)
    p[i++] = 0;                                  // conidx (reserved)
    p[i++] = s_first_pair ? 1 : 0;               // verify_mode
    // verify_data u16: on a first pairing a random 4-digit code, as DJI's demo and the working debug
    // tool send ("first pairing: the camera shows a code pop-up"). With 0 the Action 6 showed no
    // prompt at all and never answered (bench 2026-11-22). Reconnects (verify_mode 0) send 0.
    uint16_t code = s_first_pair ? (uint16_t)(1000u + esp_random() % 9000u) : 0;
    p[i++] = (uint8_t)code; p[i++] = (uint8_t)(code >> 8);
    i += 4;                                      // reserved[4]
    tx(DJI_CMD_WAIT_RESULT, CS_COMMON, ID_CONNECT, p, sizeof(p));
    applog_note("dji connect req id=%08lx verify=%d code=%u", (unsigned long)s_device_id, s_first_pair, code);
}

static void send_subscribe(void)
{
    // verified: camera_status_subscription_command_frame + docs/protocol_data_segment.md
    // "Camera Status Subscription (1D05)" @ 92fe23e5 (2026-08-29):
    //   push_mode u8 | push_freq u8 | reserved u8[4]
    //   push_mode 2 = Periodic (3 = Periodic + on-change).
    //   push_freq: units 0.1 Hz, ONLY 20 is accepted -> fixed 2 Hz (PUSH_FREQ_2HZ).
    //   cmd_type CMD_NO_RESPONSE (status_logic.c:132); the "response" is the 1D02 push.
    uint8_t p[6] = { 2, 20, 0, 0, 0, 0 };
    tx(DJI_CMD_NO_RESPONSE, CS_CAMERA, ID_SUBSCRIBE, p, sizeof(p));
}

// ---- ops ---------------------------------------------------
// §7.4 record fallback: some Osmo firmware ignores the 1D03 record-control cmd
// in certain modes. If the same record request is re-issued while still
// unconfirmed (ctrl_task retry), also press the physical Record key.
static bool     s_rec_want;          // last requested record target
static bool     s_rec_confirmed;     // camera has reported matching state since
static bool     s_key_fallback_used; // hard-key toggled once for this intent
static uint32_t s_rec_sent_ms;       // when this intent was first issued (0 = none)

// Fold a status-push observation into the record-confirm latch. Called from
// parse_status_push once it has decided `rec`.
static void dji_rec_observe(tribool_t rec)
{
    if (!s_rec_sent_ms || s_rec_confirmed) { return; }
    if (rec == (s_rec_want ? TRIBOOL_TRUE : TRIBOOL_FALSE)) { s_rec_confirmed = true; }
}

static void dji_key_report_record(void)
{
    // verified: key_report_command_frame + enums_logic.h KEY_CODE (Osmo Record
    // hard-key = 0x01), mode 0x01 report-event, key_value 0x00 short-press
    // @ 92fe23e5. The Record key is a TOGGLE with no start/stop param -- it may
    // only be pressed once per intent, and only after the primary 1D03 command
    // has demonstrably not taken (see dji_record).
    uint8_t p[4] = { 0x01, 0x01, 0x00, 0x00 };
    tx(DJI_CMD_RESPONSE_OPT, CS_COMMON, ID_KEY_REPORT, p, sizeof(p));
    applog_note("dji record: 1D03 unconfirmed 2.5s -> hard-key fallback (once)");
}

static esp_err_t dji_record(bool start)
{
    uint32_t now = now_ms();
    bool new_intent = (s_rec_want != start) || !s_rec_sent_ms;
    if (new_intent) {
        s_rec_want = start;
        s_rec_confirmed = false;
        s_key_fallback_used = false;
        s_rec_sent_ms = now ? now : 1;
    }

    uint8_t p[9]; memset(p, 0, sizeof(p));
    put_u32(p, s_device_id);
    p[4] = start ? 0 : 1;             // record_ctrl: 0 start, 1 stop
    // verified: record_control_command_frame (device_id u32 | record_ctrl u8 |
    // reserved u8[4]) @ 92fe23e5; cmd_type CMD_RESPONSE_OR_NOT (command_logic.c).
    pend_add(tx(DJI_CMD_RESPONSE_OPT, CS_CAMERA, ID_REC, p, sizeof(p)),
             start ? PEND_REC_START : PEND_REC_STOP);

    // §7.4 fallback: fire the hard-key toggle only when this same intent has
    // gone unconfirmed by the 2 Hz status push for >= 2.5 s, and only once.
    // (A toggle fired on a retry that raced a slow ACK would invert the clip.)
    if (!new_intent && !s_rec_confirmed && !s_key_fallback_used &&
        (now - s_rec_sent_ms) >= 2500) {
        dji_key_report_record();
        s_key_fallback_used = true;
    }
    return ESP_OK;
}

static esp_err_t dji_select_preset(uint8_t slot)
{
    const cfg_t *c = cfg_get();
    if (slot >= cfg_zones(c)) { return ESP_ERR_INVALID_ARG; }
    uint32_t zid = cfg_zone_id(c, slot);            // DJI: zone id == mode value
    if (zid > 0xFF) {
        // A camera mode is one byte. A bigger id is not a DJI mode: typically a GoPro preset id that a
        // newly paired DJI inherited with the other camera's zones. Never truncate it into a random mode.
        applog_note("dji zone %u: id %lu is not a camera mode -- not sent", slot + 1, (unsigned long)zid);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mode = (uint8_t)zid;
    uint8_t p[9]; memset(p, 0, sizeof(p));
    put_u32(p, s_device_id);
    p[4] = mode;
    // verified: camera_mode_switch_command_frame (device_id u32 | mode u8 |
    // reserved u8[4]) @ 92fe23e5; cmd_type CMD_RESPONSE_OR_NOT (command_logic.c).
    // `mode` values: see dji_mode_str() / camera_mode_t.
    pend_add(tx(DJI_CMD_RESPONSE_OPT, CS_CAMERA, ID_MODE, p, sizeof(p)), PEND_PRESET);
    return ESP_OK;
}

// verified: dji-sdk/Osmo-GPS-Controller-Demo logic/gps_logic.c:517-518,
// docs/protocol_data_segment.md "GPS Data Push (0017)", protocol/
// dji_protocol_data_structures.h  @ 92fe23e5a749f189593f980a26a105c3bb66aa1c
// (2026-08-29):
//   year_month_day     i32 = year*10000 + month*100 + day        (from UTC date)
//   hour_minute_second i32 = (hour + 8)*10000 + minute*100 + sec  (hour is the
//     UTC hour from NMEA; the +8 is a fixed part of the encoding. No date/hour
//     rollover is applied by the reference impl, so the field may hold hour>23.)
// The camera applies its own configured time-zone to this UTC+8 reference, so
// the displayed clock is only correct if the camera's TZ setting is correct --
// this protocol carries no TZ field. (UNRESOLVED: needs a real-camera capture
// to confirm displayed-time behaviour for a non-UTC+8 user.)
static void fill_datetime(uint8_t *p8, int64_t utc_ms)
{
    time_t t = (time_t)(utc_ms / 1000);
    struct tm tm; gmtime_r(&t, &tm);
    int32_t ymd = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    int32_t hms = (tm.tm_hour + 8) * 10000 + tm.tm_min * 100 + tm.tm_sec;
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
    // verified: gps_data_push_command_frame layout (ymd i32 | hms i32 | lon i32 e7
    // | lat i32 e7 | height i32 mm | vN/vE/vD f32 cm/s | vAcc/hAcc u32 mm |
    // sAcc u32 cm/s | sats u32) @ 92fe23e5. NOTE longitude precedes latitude.
    // cmd_type CMD_NO_RESPONSE (command_logic.c:180-184; ack not required).
    tx(DJI_CMD_NO_RESPONSE, CS_COMMON, ID_GPS_PUSH, p, i);
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
    // A DJI Osmo has no highlight tag. The only frame this used to send was the QS key report
    // (key_code 0x02, mode 0x01, value 0x00 @ 92fe23e5), and QS is the "quick-switch mode" button:
    // on a real Action 6 it switched Video -> Photo (bench 2026-11-21). A mode change nobody asked
    // for is worse than no tag, so a DJI does nothing here and the portal hides the setting.
    return ESP_ERR_NOT_SUPPORTED;
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
    // FFF4 AND FFF5: the camera may answer on either, and the working debug tool subscribes to every
    // notifiable characteristic. FFF5 is also the write characteristic (read/write-no-rsp/notify/indicate).
    static const brand_gatt_t g = { .sub = { &U_NOTIFY.u, &U_WRITE.u, NULL, NULL }, .sub_n = 2 };
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
    duml_reasm_reset(&rz5);
    s_dbg_n = 0;
    memset(s_pend, 0, sizeof(s_pend));
    s_txt_valid = false; s_model[0] = '\0'; s_asleep = false;
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
    s_rec_sent_ms = 0;
    s_rec_confirmed = false;
    s_key_fallback_used = false;
}

// ---- status push parsing (spec 7.5) --------------------
// verified: dji-sdk/Osmo-GPS-Controller-Demo docs/protocol_data_segment.md
// "Camera Status Push (1D02)" + protocol/dji_protocol_data_structures.h
// (camera_status_push_command_frame, __attribute__((packed)), 38 bytes)
// @ 92fe23e5a749f189593f980a26a105c3bb66aa1c (2026-08-29). Field offsets:
//   0  camera_mode u8      1  camera_status u8    2  video_resolution u8
//   3  fps_idx u8          4  EIS_mode u8         5  record_time u16
//   7  fov_type u8         8  photo_ratio u8      9  real_time_countdown u16
//   11 timelapse_interval u16   13 timelapse_duration u16
//   15 remain_capacity u32 (MB) 19 remain_photo_num u32   23 remain_time u32 (s)
//   27 user_mode u8        28 power_mode u8       29 camera_mode_next_flag u8
//   30 temp_over u8        31 photo_countdown_ms u32   35 loop_record_sends u16
//   37 camera_bat_percentage u8
// Push cadence is fixed 2 Hz (see send_subscribe). camera_mode > 0x34 => a 1D06
// "new camera status push" frame follows with the English mode/param strings.
// 1D/06 "new camera status push": [01][len][name x21][02][len][param x21], 46 bytes. The Action 6 sends
// it on every mode change, e.g. name "VIDEO", param "4K30 Off". The text is the camera's own wording,
// so it is the fallback for resolution / frame rate when the 1D/02 enum code is not in our table
// (an Action 6 in 4K30 reports video_resolution 125, which DJI's table does not list).

static void parse_mode_text(const uint8_t *p, uint16_t n)
{
    s_txt_valid = false;
    if (n < 46 || p[0] != 1 || p[23] != 2 || p[1] > 21 || p[24] > 21) { return; }
    char par[22]; memcpy(par, &p[25], p[24]); par[p[24]] = '\0';
    // leading "<res><K|P><fps>" token ("4K30", "2.7K60", "1080P30", "4K120"); anything else (photo
    // sizes, "M (16:9)") leaves the text unused.
    int i = 0, rl = 0;
    char res[8], fps[6];
    while (par[i] && par[i] != 'K' && par[i] != 'P' && i < 6) { i++; }
    if (par[i] != 'K' && par[i] != 'P') { return; }
    rl = i + 1;
    if (rl >= (int)sizeof(res)) { return; }
    memcpy(res, par, rl); res[rl] = '\0';
    int fl = 0;
    while (par[rl + fl] >= '0' && par[rl + fl] <= '9' && fl < 4) { fps[fl] = par[rl + fl]; fl++; }
    if (fl == 0) { return; }
    fps[fl] = '\0';
    // a resolution like "1080P" is shown as "1080" like the enum table does
    if (res[rl - 1] == 'P') { res[rl - 1] = '\0'; }
    strlcpy(s_txt_res, res, sizeof(s_txt_res));
    strlcpy(s_txt_fps, fps, sizeof(s_txt_fps));
    s_txt_valid = true;
    s_txt_mode = cam_scratch()->mode.valid ? (uint8_t)cam_scratch()->mode.v : 0xFF;   // 0xFF: adopt the next 1D/02 mode
}

static void parse_status_push(const uint8_t *p, uint16_t n)
{
    if (n < 38) { return; }
    cam_state_t *s = cam_scratch();

    uint8_t  camera_mode  = p[0];
    uint8_t  camera_status = p[1];
    uint8_t  res_idx      = p[2];
    uint8_t  fps_idx      = p[3];
    /* uint8_t eis = p[4]; */
    uint16_t record_time  = get_u16(&p[5]);
    uint32_t remain_mb    = get_u32(&p[15]);
    uint32_t remain_time  = get_u32(&p[23]);
    uint8_t  batt_pct     = p[37];
    uint8_t  power_mode   = p[28];
    /* uint8_t temp_over = p[30]; */

    if (power_mode == 3) {
        // The only documented sleep indicator (1D/02 power_mode 3). A sleeping Osmo keeps its BLE link
        // and even answers the handshake, but its numbers are not a live camera: report nothing instead
        // of values that look real, so it never "proves" itself (see cam_task reconnect policy).
        if (!s_asleep) { s_asleep = true; applog_note("dji: camera asleep (power_mode 3)"); }
        s->recording = TRIBOOL_UNKNOWN;
        s->pre_record = TRIBOOL_UNKNOWN;
        if (CB->on_state_dirty) { CB->on_state_dirty(); }
        return;
    }
    s_asleep = false;

    s->mode.valid = true; s->mode.v = camera_mode;
    val_str_set(&s->mode_name, dji_mode_str(camera_mode));
    if (!*s->mode_name.v) { s->mode_name.valid = false; }

    // recording: camera_status 0x03 = "photo or recording in progress",
    // 0x05 = "pre-recording". verified camera_mode_t @ 92fe23e5 -- video-capable
    // modes: 0x00 SlowMo, 0x01 Video, 0x02 Timelapse, 0x0A Hyperlapse,
    // 0x28 SuperNight, 0x34 Subject-Tracking (0x05 Photo / 0x1A,0x23 Live excluded).
    // FIXME(protocol-verifier): recording-detection SEMANTICS need perf-coherence
    // review -- also cross-check record_time increasing (spec 7.5); pre-record vs
    // record distinction; Osmo360 modes (>0x34) not covered here.
    bool is_video = (camera_mode == 0x00 || camera_mode == 0x01 || camera_mode == 0x02 ||
                     camera_mode == 0x0A || camera_mode == 0x28 || camera_mode == 0x34);
    tribool_t rec = (camera_status == 3 && is_video) ? TRIBOOL_TRUE :
                    (camera_status == 5 ? TRIBOOL_TRUE : TRIBOOL_FALSE);
    s->recording = rec;
    s->pre_record = (camera_status == 5) ? TRIBOOL_TRUE : TRIBOOL_FALSE;   // spec 7.5 / %st PRE
    dji_rec_observe(rec);   // §7.4: confirm record intent from the push, not the optional ACK

    s->rec_elapsed_s.valid = true; s->rec_elapsed_s.v = record_time;
    s->rec_remaining_s.valid = true; s->rec_remaining_s.v = remain_time;
    s->sd_remaining_kb.valid = true; s->sd_remaining_kb.v = remain_mb * 1024;   // MB -> KB
    s->sd_missing = (remain_mb == 0);
    s->battery_pct.valid = true; s->battery_pct.v = batt_pct;

    // enum tables first; when a code is unknown fall back to the camera's own 1D/06 text, but only
    // while that text belongs to the current mode (blank-on-unconfirmed otherwise).
    if (s_txt_valid && s_txt_mode == 0xFF) { s_txt_mode = camera_mode; }
    if (s_txt_valid && s_txt_mode != camera_mode) { s_txt_valid = false; }
    const char *rs = dji_resolution_str(res_idx);
    if (!*rs && s_txt_valid) { rs = s_txt_res; }
    val_str_set(&s->resolution, rs);
    if (!*s->resolution.v) { s->resolution.valid = false; }
    const char *fs = dji_fps_str(fps_idx);
    if (!*fs && s_txt_valid) { fs = s_txt_fps; }
    val_str_set(&s->framerate, fs);
    if (!*s->framerate.v) { s->framerate.valid = false; }

    s->busy = TRIBOOL_FALSE;   // DJI has no explicit busy; mode switch latency handled by ctrl dwell

    if (CB->on_state_dirty) { CB->on_state_dirty(); }   // commits + wakes osd_task
}

static void dji_go_ready(void)
{
    if (s_handshaked) { return; }   // idempotent: response + command frame can both arrive
    s_handshaked = true;
    send_subscribe();
    if (s_first_pair && CB->on_paired) {
        const cam_state_t *s = cam_scratch();
        CB->on_paired(s->addr, s->name[0] ? s->name : (s_model[0] ? s_model : "DJI Osmo"), s_device_id);
        s_first_pair = false;
    }
    if (CB->on_link_state) { CB->on_link_state(CAM_LINK_READY); }
}

static void handle_msg(const duml_msg_t *m)
{
    if (m->cmd_set == CS_COMMON && m->cmd_id == ID_CONNECT) {
        bool is_response = (m->cmd_type & 0x20) != 0;

        if (is_response) {
            // [device_id u32][ret_code u8]. On first pair this only means the
            // camera accepted our request -- the camera then sends its own
            // COMMAND frame once the user confirms on-screen (verify_mode 2).
            uint8_t ret = (m->payload_len >= 5) ? m->payload[4] : 0xFF;
            ESP_LOGI(TAG, "connect resp ret=%u (first_pair=%d)", ret, s_first_pair);
            if (ret != 0) { applog_note("dji connect rejected ret=%u", ret); return; }
            if (!s_first_pair) { dji_go_ready(); }
            // Reconnect: we go READY on the response so a push-happy firmware
            // works immediately; if the camera still sends its 0x19 command
            // frame we ACK it in the branch below (dji_go_ready is idempotent).
            // First pair: nothing is READY yet -- wait for that command frame.
            return;
        }

        // Camera-initiated COMMAND frame: connection_request_command_frame.
        // verify_mode @ 26 (must be 2 = result), verify_data u16 LE @ 27
        // (0 = user confirmed), reserved[0] @ 29. Verified: connect_logic.c
        // @ 92fe23e5.
        if (m->payload_len < DJI_CONN_REQ_LEN) { applog_note("dji: short verify frame"); return; }
        uint8_t  verify_mode = m->payload[26];
        uint16_t verify_data = m->payload[27] | ((uint16_t)m->payload[28] << 8);
        uint8_t  cam_reserved = m->payload[29];
        ESP_LOGI(TAG, "dji verify: mode=%u data=%u", verify_mode, verify_data);
        {
            // The camera's own device id names the model (docs: Action 4 FF33, Action 5 Pro FF44,
            // Action 6 FF55, Osmo 360 FF66; the sample frame carries them as 00 00 33 FF).
            uint32_t id = get_u32(m->payload);
            uint16_t hi = (uint16_t)(id >> 16) == 0 ? (uint16_t)(id & 0xFFFF) : (uint16_t)(id >> 16);
            const char *mn = NULL;
            switch (hi) {
            case 0xFF33: mn = "Osmo Action 4"; break;
            case 0xFF44: mn = "Osmo Action 5 Pro"; break;
            case 0xFF55: mn = "Osmo Action 6"; break;
            case 0xFF66: mn = "Osmo 360"; break;
            default: break;
            }
            if (mn) { strlcpy(s_model, mn, sizeof(s_model)); applog_note("dji model: %s", mn); }
        }

        if (verify_mode == 2 && verify_data == 0) {
            uint8_t rsp[9] = {0};
            rsp[0] = s_device_id; rsp[1] = s_device_id >> 8;
            rsp[2] = s_device_id >> 16; rsp[3] = s_device_id >> 24;
            rsp[4] = 0;                          // ret_code OK
            rsp[5] = cam_reserved;               // echo reserved[0]
            tx_ack(m->seq, CS_COMMON, ID_CONNECT, rsp, sizeof(rsp));
            applog_note("dji pairing confirmed");
            dji_go_ready();
        } else {
            applog_note("dji pairing NOT confirmed (mode=%u data=%u)", verify_mode, verify_data);
            // stay SYNCING -> cam_task rescans on CONNECT_TIMEOUT_MS
        }
    } else if (m->cmd_set == CS_CAMERA && m->cmd_id == ID_STATUS_PUSH) {
        parse_status_push(m->payload, m->payload_len);
    } else if (m->cmd_set == CS_CAMERA && m->cmd_id == ID_STATUS_PUSH2) {
        parse_mode_text(m->payload, m->payload_len);
    } else if (m->cmd_set == CS_CAMERA && (m->cmd_id == ID_REC || m->cmd_id == ID_MODE)) {
        if (!(m->cmd_type & 0x20)) { return; }   // only replies, not a camera-initiated command
        // Match by seq, ignore the cmd id (see pend_take). Not ours / already answered: ignore.
        pend_op_t op = pend_take(m->seq);
        if (op == PEND_NONE) { return; }
        uint8_t ret = m->payload_len ? m->payload[0] : 0xFF;
        // Note: the reply is optional (CMD_RESPONSE_OR_NOT); the status-push confirm in
        // dji_rec_observe() is the primary signal (§7.4).
        if ((op == PEND_REC_START || op == PEND_REC_STOP) && ret == 0 && (op == PEND_REC_START) == s_rec_want) {
            s_rec_confirmed = true;
        }
        if (CB->on_ack) {
            CB->on_ack(op == PEND_REC_START ? CAM_OP_RECORD_START
                       : op == PEND_REC_STOP ? CAM_OP_RECORD_STOP : CAM_OP_SELECT_PRESET, ret == 0);
        }
    }
}

static void dji_on_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    // The camera answers on FFF4 and (some models/frames) on FFF5; each characteristic has its own
    // reassembly buffer so two streams can never splice into one frame.
    duml_reasm_t *r = handle == vh_notify ? &rz : (handle == h_write && h_write) ? &rz5 : NULL;
    if (!r) { return; }
    if (s_dbg_n < 6) {                          // first few frames per link: what arrives, and where (pairing bring-up)
        char hx[3 * 16 + 1]; int k = 0;
        for (uint16_t i = 0; i < len && i < 16; i++) { k += snprintf(hx + k, sizeof(hx) - k, "%02x ", data[i]); }
        ESP_LOGI(TAG, "notify %s len=%u: %s", r == &rz ? "fff4" : "fff5", len, hx);
        s_dbg_n++;
    }
    uint32_t now = now_ms();
    // A half frame whose tail never came would swallow the start of the next one: drop it after 2 s.
    if ((rz.have || rz5.have) && (int32_t)(now - s_last_rx_ms) > 2000) { duml_reasm_reset(&rz); duml_reasm_reset(&rz5); }
    s_last_rx_ms = now;
    duml_msg_t m;
    if (duml_reasm_feed(r, data, len, &m)) { handle_msg(&m); }
    while (duml_reasm_next(r, &m)) { handle_msg(&m); }
}

// ---- adv match (spec 7.1) ---------------------------------
static bool dji_adv_match(const ble_adv_t *adv)
{
    // Strong: manufacturer data AA 08 <model> 00 FA ... (company id 0x08AA). On an Action 6 this sits in
    // the SCAN RESPONSE, not in the advert that carries the name, and a passive scan (portal up) never
    // sees it -- so the advert itself is also matched weakly, by name or service (hardware-observed:
    // "OsmoAction6-XXXX"; DJI documents short names "OA4" / "OA5Pro"). Plain "DJI*" names are NOT
    // matched: drones and controllers use them and must not appear in the camera list.
    if (adv->mfg_len >= 5 && adv->mfg[0] == 0xAA && adv->mfg[1] == 0x08 && adv->mfg[4] == 0xFA) {
        return true;
    }
    if (adv->name[0] && (strncmp(adv->name, "Osmo", 4) == 0 || strncmp(adv->name, "OA", 2) == 0 ||
                         strncmp(adv->name, "Xtra", 4) == 0)) {
        return true;
    }
    return adv->svc_uuid16 == 0xFFF0;
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
