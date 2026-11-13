// gopro_driver.c -- Open GoPro BLE driver (spec 6). Runs entirely in cam_task
// context. Every camera write is fire-and-forget (spec 1.2.4); results surface
// asynchronously via status/setting notifications.
#include "camera_gopro.h"
#include "gopro_tlv.h"
#include "gopro_enums.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "gp_preset.h"
#include "cam_caps.h"
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
       ST_PRESET_GROUP = 96 };   // 1000 VIDEO / 1001 PHOTO / 1002 TIMELAPSE
enum { SET_RESOLUTION = 2, SET_FPS = 3, SET_ASPECT = 108, SET_LENS = 121,
       SET_BIT_DEPTH = 183, SET_PROFILE = 184 };
// Unofficial Video + Protune settings (not in the Open GoPro spec; IDs/options
// from the HERO11 gpControl dump, gopro_ble_debug/docs App. D). Shown on the
// portal status page only. 13/102/145 are the VIDEO-mode ISO/shutter -- photo
// and multishot have their own IDs -- so they are ignored outside VIDEO.
enum { SET_ISO_MAX = 13, SET_ISO_MIN = 102, SET_SHUTTER = 145, SET_PROTUNE = 114,
       SET_WB = 115, SET_COLOR = 116, SET_SHARPNESS = 117, SET_EV = 118,
       SET_10BIT = 174 };

// Statuses the driver consumes: registered for pushes and polled by id.
static const uint8_t k_st_ids[] = { ST_ENCODING, ST_BUSY, ST_HOT, ST_BATT_PCT,
                                    ST_SD_REMAIN_KB, ST_REMAIN_VIDEO_S,
                                    ST_SD_STATUS, ST_PRESET_GROUP };
static bool poll_full;               // the camera refused a targeted poll: use get-all instead
static uint8_t poll_err;

// ---- driver state ---------------------------------------------------
static const camera_cbs_t *CB;
static uint16_t h_cmd, h_set, h_qry;      // write value handles
static uint16_t vh_cmd_rsp, vh_set_rsp, vh_qry_rsp;
static gopro_reasm_t rz_cmd, rz_set, rz_qry;
// Reassembly buffers sized per characteristic: the query characteristic carries get-all
// replies (~1 KB at most); command and settings replies are tiny. (Was 3 x 1 KB.)
static uint8_t rzb_cmd[256], rzb_set[64], rzb_qry[1024];
static uint32_t last_keepalive_ms;
static uint32_t rec_start_ms;
static uint8_t  getall_retry;
static uint32_t last_getall_ms;      // get-all-settings poll
static uint32_t last_status_poll_ms; // get-all-status poll

static void gp_pull_all(void);
static void caps_request(uint32_t delay_ms);
static bool     want_record_ack;
static bool     want_record_target;

// Open GoPro requires Get Hardware Info (0x3C) to be polled until it answers
// with status 0 before ANY other command is sent -- that reply is the camera's
// "BLE ready" signal. Until then this driver sends nothing but 0x3C retries;
// sending queries earlier is why registration/get-alls could land before the
// camera was listening. (Separate from cam_task's CAM_LINK_READY, which is the
// link lifecycle state and must be reported promptly so ticks start running.)
static bool     ble_ready;
static bool     hw_ok;               // a Get Hardware Info reply with status 0 has been seen
static bool     aspect108_seen;      // setting 108 (real aspect ratio) was reported this link
static uint32_t last_hwinfo_ms;
static uint32_t hwinfo_first_ms;
static uint16_t last_reasm_drops;
#define HWINFO_RETRY_MS 500
// Fail open: a camera that never answers 0x3C (unsupported, or an unexpected
// status) must not leave us silent forever -- that would mean no telemetry at
// all, which is worse than talking to a camera that may not be listening yet.
// Give up waiting after this and carry on, loudly.
#define HWINFO_GIVEUP_MS 3000
// After giving up we keep asking, slowly: when the camera finally answers, registration is
// done again (what was sent to a not-yet-listening camera is lost for the whole session).
#define HWINFO_LATE_RETRY_MS 2000

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
// Splits the message into Open GoPro packets (<= 20 B each) and queues them.
// ble_central serializes the queue, so a multi-packet message is delivered in
// order and one ATT request at a time -- required, because ATT allows a single
// outstanding request and the camera reassembles strictly in sequence.
static void wr(uint16_t handle, const uint8_t *body, uint16_t n)
{
    if (!handle) { return; }
    gopro_tx_t t;
    gopro_tx_init(&t, body, n);
    uint8_t pkt[GOPRO_PKT_MAX];
    size_t len;
    while ((len = gopro_tx_next(&t, pkt, sizeof(pkt))) > 0) {
        if (ble_central_write(handle, pkt, len, true) != ESP_OK) { break; }
    }
}

// ---- which 0x12 request does a 0x12 reply answer? ------------------------------
// Replies on the query characteristic are matched by query id only, but we send
// several 0x12 requests (periodic poll, zone read-back, initial get-all) and the
// camera answers a characteristic's requests in order. Remember the order we sent
// them in, so a reply is attributed to the request that asked for it instead of
// guessing from its length.
enum { Q12_POLL = 0, Q12_VERIFY = 1 };
static uint8_t  q12_fifo[4], q12_n;
static uint32_t q12_t_ms;
static void q12_push(uint8_t tag)
{
    if (q12_n == sizeof(q12_fifo)) { memmove(q12_fifo, q12_fifo + 1, --q12_n); }
    if (!q12_n) { q12_t_ms = now_ms(); }
    q12_fifo[q12_n++] = tag;
}
static uint8_t q12_pop(void)
{
    if (!q12_n) { return Q12_POLL; }                 // unsolicited: treat like a poll
    uint8_t t = q12_fifo[0];
    memmove(q12_fifo, q12_fifo + 1, --q12_n);
    q12_t_ms = now_ms();
    return t;
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

// ---- capabilities: what each setting may be set to RIGHT NOW ----------------
// Open GoPro "Get Setting Capabilities" (query 0x32 <id>): the reply repeats the
// id once per allowed option, and the list depends on the camera's other
// settings (the fps list on the current resolution, ...). One request per id,
// one at a time -- so a setting the model lacks only costs its own empty reply
// instead of failing a combined request. Afterwards register for capability
// pushes (0x62) for the settings that answered, so later changes arrive by
// themselves; a re-ask is also scheduled after every change we make, since
// pushes are not yet confirmed on real cameras. Published to cam_caps for the
// portal's preset builder.
enum { CP_IDLE, CP_WAIT };
#define CP_TIMEOUT_MS 2000
static struct {
    uint8_t  state;
    int8_t   cur;
    bool     want, registered;
    uint32_t due_ms, t_ms;
} cp;
static cam_caps_t wk;       // working copy; published when a pass completes

static void caps_request(uint32_t delay_ms) { cp.want = true; cp.due_ms = now_ms() + delay_ms; }

static void caps_register(void)
{
    uint8_t b[1 + GP_PSET_MAX];
    int n = 0;
    b[0] = 0x62;                                    // register capability updates
    for (int i = 0; i < GP_PSET_MAX; i++) {
        if (wk.have[i]) { b[1 + n++] = gp_pset_id(i); }
    }
    if (n) { wr(h_qry, b, (uint16_t)(1 + n)); }
    cp.registered = true;
}

static void caps_next(void)
{
    cp.cur++;
    if (cp.cur >= GP_PSET_MAX) {
        cp.state = CP_IDLE;
        cam_caps_publish(&wk);
        if (!cp.registered) { caps_register(); }
        return;
    }
    uint8_t b[2] = { 0x32, gp_pset_id(cp.cur) };
    wr(h_qry, b, 2);
    cp.state = CP_WAIT;
    cp.t_ms = now_ms();
}

static void caps_pump(void)
{
    uint32_t t = now_ms();
    if (cp.state == CP_WAIT) {
        if (t - cp.t_ms >= CP_TIMEOUT_MS) { caps_next(); }          // no reply: keep what we had, move on
        return;
    }
    // Only the portal's builder reads the capability lists, so passes (16 sequential
    // queries each) run only while it is up; a request made earlier stays pending.
    if (cp.want && ble_ready && cam_portal_up() && (int32_t)(t - cp.due_ms) >= 0) {
        cp.want = false;
        cp.cur = -1;
        caps_next();
    }
}

// TLVs of a 0x32 reply (one id), a 0x62 register echo or a 0xA2 push:
// [id][len][value], the id repeating once per allowed option.
static void caps_on_tlv(uint8_t qid, const uint8_t *p, uint16_t len)
{
    bool seen[GP_PSET_MAX] = { false };
    bool any = false;                       // the reply carried at least one tuple, for any id
    for (uint16_t i = 0; i + 2 <= len; ) {
        uint8_t id = p[i], n = p[i + 1];
        i += 2;
        if (i + n > len) { break; }
        if (n >= 1) { any = true; }
        int k = gp_pset_index(id);
        bool mine = k >= 0 && n >= 1 && (qid != 0x32 || (cp.state == CP_WAIT && k == cp.cur));
        if (mine) {
            if (!seen[k]) { seen[k] = true; memset(wk.bits[k], 0, sizeof(wk.bits[k])); }
            wk.bits[k][p[i] >> 3] |= (uint8_t)(1u << (p[i] & 7));
            wk.have[k] = true;
        }
        i += n;
    }
    if (qid == 0x32) {
        if (cp.state != CP_WAIT) { return; }
        if (!seen[cp.cur]) {
            // Tuples for some OTHER id = a late reply to a query we already timed out on:
            // ignore it and keep waiting for ours. Only an empty reply means "not offered".
            if (any) { return; }
            wk.have[cp.cur] = false; memset(wk.bits[cp.cur], 0, sizeof(wk.bits[cp.cur]));
        }
        caps_next();
    } else {
        cam_caps_publish(&wk);
    }
}

// ---- zone presets as settings ---------------------------------------------
// A zone does not load a GoPro preset: cfg.gp_preset[zone] lists (setting id,
// option value) pairs and we write them to the camera's active preset. One
// write at a time, in gp_preset.h order (resolution before the settings whose
// options depend on it), each acknowledged by the camera's [id][status] reply
// on the settings-response characteristic. Then one read-back of everything we
// wrote; whatever did not stick is rewritten once, and the rest is reported
// (cam_state gp_apply_*) rather than assumed. Settings the camera already has
// are not rewritten. All of it runs in cam_task context and never blocks.
//
// Video settings only apply to a VIDEO preset, so the apply waits until the
// camera is in one (status 96) instead of firing writes that would be refused.
enum { AP_IDLE, AP_WAIT, AP_SEND, AP_HOLD, AP_VERIFY };
enum { APS_PEND, APS_SENT, APS_REJ, APS_BAD };   // BAD = accepted, not confirmed
#define AP_WRITE_TIMEOUT_MS   3000
#define AP_VERIFY_TIMEOUT_MS  2000
#define AP_MODE_WAIT_MS       4000   // fail open if the preset group never shows up
// Open GoPro state_management: wait for System Busy (status 8) to clear before sending
// anything but queries -- a resolution change keeps the camera busy and the next write is
// refused. Also: status 0 on a setting write means "accepted", not "applied", so let the
// camera settle before the next write and before the read-back. Both fail open.
#define AP_SETTLE_MS          300
#define AP_BUSY_WAIT_MS       3000

static struct {
    uint8_t  state, slot, pass, n;
    int8_t   cur;                       // entry awaiting its write ack
    uint8_t  idx[GP_PSET_MAX];          // index into gp_pset_id()
    uint8_t  val[GP_PSET_MAX];
    uint8_t  st[GP_PSET_MAX];
    uint8_t  tries[GP_PSET_MAX];        // rejected writes already retried
    uint32_t t_ms;                      // when the current phase started
} ap;

static void ap_publish(uint8_t state, uint16_t fail)
{
    cam_state_t *s = cam_scratch();
    s->gp_apply_zone = (uint8_t)(ap.slot + 1);
    s->gp_apply_state = state;
    s->gp_apply_fail = fail;
    if (CB && CB->on_state_dirty) { CB->on_state_dirty(); }
}

static void ap_finish(void)
{
    uint16_t fail = 0;
    int bad = 0;
    char ids[GP_PSET_MAX * 4 + 1]; ids[0] = '\0';
    for (int i = 0; i < ap.n; i++) {
        if (ap.st[i] == APS_REJ || ap.st[i] == APS_BAD) {
            fail |= (uint16_t)(1u << ap.idx[i]);
            bad++;
            size_t l = strlen(ids);
            snprintf(ids + l, sizeof(ids) - l, " %u", gp_pset_id(ap.idx[i]));
        }
    }
    ap.state = AP_IDLE;
    ap_publish(bad == 0 ? GPA_OK : bad == ap.n ? GPA_FAILED : GPA_PARTIAL, fail);
    applog_note("gopro zone %u: %d/%d settings confirmed%s%s", ap.slot + 1, ap.n - bad, ap.n,
                bad ? " -- not applied:" : "", ids);
    if (CB && CB->on_ack) { CB->on_ack(CAM_OP_SELECT_PRESET, bad == 0); }
    caps_request(300);          // the camera's options changed with its settings
}

// Read back every setting we wrote (one get-values request with the id list).
static void ap_request_verify(void)
{
    uint8_t b[1 + GP_PSET_MAX];
    int n = 0;
    b[0] = 0x12;                                    // get setting values
    for (int i = 0; i < ap.n; i++) {
        if (ap.st[i] != APS_REJ) { b[1 + n++] = gp_pset_id(ap.idx[i]); }
    }
    if (n == 0) { ap_finish(); return; }
    wr(h_qry, b, (uint16_t)(1 + n));
    q12_push(Q12_VERIFY);
    ap.state = AP_VERIFY;
    ap.t_ms = now_ms();
}

// Pause before the next write / the read-back: let the camera settle and, if it is
// busy, wait (capped) until it is not. ap_pump() ends the hold.
static void ap_hold(void)
{
    ap.cur = -1;
    ap.state = AP_HOLD;
    ap.t_ms = now_ms();
}

// Send the next setting that is not already on the camera; verify when none is left.
static void ap_next(void)
{
    cam_state_t *s = cam_scratch();
    for (int i = 0; i < ap.n; i++) {
        if (ap.st[i] != APS_PEND) { continue; }
        int k = ap.idx[i];
        if (s->gp_set[k].valid && s->gp_set[k].v == ap.val[i]) { ap.st[i] = APS_SENT; continue; }
        uint8_t b[3] = { gp_pset_id(k), 0x01, ap.val[i] };
        wr(h_set, b, 3);
        ap.cur = (int8_t)i;
        ap.state = AP_SEND;
        ap.t_ms = now_ms();
        return;
    }
    ap.cur = -1;
    ap_request_verify();
}

static void ap_evaluate(void)
{
    cam_state_t *s = cam_scratch();
    bool retry = false;
    for (int i = 0; i < ap.n; i++) {
        if (ap.st[i] == APS_REJ) { continue; }
        int k = ap.idx[i];
        if (s->gp_set[k].valid && s->gp_set[k].v == ap.val[i]) { ap.st[i] = APS_SENT; continue; }
        if (ap.pass == 0) { ap.st[i] = APS_PEND; retry = true; }   // e.g. a later write undid it
        else              { ap.st[i] = APS_BAD; }
    }
    if (retry) { ap.pass = 1; ap_hold(); }
    else       { ap_finish(); }
}

// Reply to one of our setting writes: [id][status], status 0 = accepted.
static void ap_on_set_rsp(uint8_t id, uint8_t status)
{
    if (ap.state != AP_SEND || ap.cur < 0 || gp_pset_id(ap.idx[ap.cur]) != id) { return; }
    if (status == 0) {
        ap.st[ap.cur] = APS_SENT;
    } else if (ap.tries[ap.cur] == 0) {
        ap.tries[ap.cur] = 1;                   // a refusal is often "busy": try once more after the hold
        ap.st[ap.cur] = APS_PEND;
        ESP_LOGI(TAG, "zone %u: setting %u refused (status %u), retrying", ap.slot + 1, id, status);
    } else {
        ap.st[ap.cur] = APS_REJ;
        applog_note("gopro zone %u: setting %u rejected (status %u)", ap.slot + 1, id, status);
    }
    ap_hold();
}

static void ap_pump(void)
{
    uint32_t t = now_ms();
    switch (ap.state) {
    case AP_WAIT: {
        if (!ble_ready) { return; }
        const cam_state_t *s = cam_scratch();
        if (!s->mode.valid && t - ap.t_ms < AP_MODE_WAIT_MS) { return; }
        if (s->mode.valid && s->mode.v != 0) { return; }       // photo / timelapse: wait for video
        if (s->busy == TRIBOOL_TRUE && t - ap.t_ms < AP_BUSY_WAIT_MS) { return; }
        ap.pass = 0;
        ap_publish(GPA_APPLYING, 0);
        ap_next();
        break;
    }
    case AP_SEND:
        if (t - ap.t_ms >= AP_WRITE_TIMEOUT_MS) {
            applog_note("gopro zone %u: setting %u: no reply", ap.slot + 1,
                        gp_pset_id(ap.idx[ap.cur]));
            ap.st[ap.cur] = APS_REJ;
            ap_hold();
        }
        break;
    case AP_HOLD: {
        const cam_state_t *s = cam_scratch();
        uint32_t e = t - ap.t_ms;
        if (e >= AP_SETTLE_MS && (s->busy != TRIBOOL_TRUE || e >= AP_BUSY_WAIT_MS)) { ap_next(); }
        break;
    }
    case AP_VERIFY:
        if (t - ap.t_ms >= AP_VERIFY_TIMEOUT_MS) { ap_evaluate(); }
        break;
    default:
        break;
    }
}

// One preset-able setting, right now (portal builder keeps the camera in step
// with what is being edited, so the capability lists match). Explicit portal
// action only; refused while recording. The writes are queued and sent one at a
// time, in order, once the camera is not busy (a resolution change keeps it busy
// and the next write would be refused) -- see lw_pump(). Re-asks the capabilities
// after each.
#define LW_MAX 8
static struct { uint8_t id, val; } lw[LW_MAX];
static uint8_t  lw_n;
static uint32_t lw_since_ms, lw_next_ms;

static esp_err_t gp_set_setting(uint8_t id, uint8_t val)
{
    if (gp_pset_index(id) < 0) { return ESP_ERR_INVALID_ARG; }
    if (!h_set || !ble_ready) { return ESP_ERR_INVALID_STATE; }
    if (cam_scratch()->recording == TRIBOOL_TRUE) {
        applog_note("gopro: setting %u not changed while recording", id);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t i = 0; i < lw_n; i++) {                       // newest value of a setting wins
        if (lw[i].id == id) { lw[i].val = val; return ESP_OK; }
    }
    if (lw_n == LW_MAX) { memmove(lw, lw + 1, (LW_MAX - 1) * sizeof(lw[0])); lw_n--; }
    if (!lw_n) { lw_since_ms = now_ms(); }
    lw[lw_n].id = id; lw[lw_n].val = val; lw_n++;
    return ESP_OK;
}

static void lw_pump(void)
{
    if (!lw_n || !ble_ready || ap.state != AP_IDLE) { return; }
    uint32_t t = now_ms();
    if ((int32_t)(t - lw_next_ms) < 0) { return; }
    const cam_state_t *s = cam_scratch();
    if (s->recording == TRIBOOL_TRUE) { lw_n = 0; return; }    // never change capture settings mid-clip
    if (s->busy == TRIBOOL_TRUE && t - lw_since_ms < AP_BUSY_WAIT_MS) { return; }
    uint8_t b[3] = { lw[0].id, 0x01, lw[0].val };
    wr(h_set, b, 3);
    memmove(lw, lw + 1, --lw_n * sizeof(lw[0]));
    lw_next_ms = t + 250;
    lw_since_ms = t;
    caps_request(400);
}

// Called by cam_task on every pass (<= 100 ms) while READY: the timer-driven parts
// (waiting for the camera, queued writes, capability passes) that must not wait for
// the slow poll tick.
static void gp_tick_fast(void)
{
    static bool portal_prev;
    ap_pump();
    lw_pump();
    caps_pump();
    bool pu = cam_portal_up();
    if (pu && !portal_prev) { caps_request(0); }                // portal just opened: the builder needs fresh lists
    portal_prev = pu;
}

static esp_err_t gp_select_preset(uint8_t slot)
{
    const cfg_t *c = cfg_get();
    if (!c || slot >= cfg_zones(c)) { return ESP_ERR_INVALID_ARG; }
    const cfg_gp_zone_t *gz = cfg_zone_gp(c, slot);

    memset(&ap, 0, sizeof(ap));
    ap.slot = slot;
    ap.cur = -1;
    // Entries in apply order, whatever order cfg stored them in.
    for (int k = 0; k < GP_PSET_MAX; k++) {
        for (int j = 0; j < gz->n && j < GP_PSET_MAX; j++) {
            if (gz->s[j].id == gp_pset_id(k)) {
                ap.idx[ap.n] = (uint8_t)k;
                ap.val[ap.n] = gz->s[j].val;
                ap.n++;
                break;
            }
        }
    }
    if (ap.n == 0) {
        ap.state = AP_IDLE;
        ap_publish(GPA_NONE, 0);
        applog_note("gopro zone %u has no camera settings -- nothing pushed", slot + 1);
        return ESP_OK;
    }
    ap.state = AP_WAIT;
    ap.t_ms = now_ms();
    ap_publish(GPA_WAITING, 0);
    applog_note("gopro zone %u: pushing %u setting(s)", slot + 1, ap.n);
    ap_pump();
    return ESP_OK;
}

// Older firmware (HERO9/10) may not know the DST-aware date-time commands. When 0x0F / 0x10
// is refused we switch to the 7-byte no-timezone pair 0x0D / 0x0E (local time only) for the
// rest of the session; the offset we last wrote is remembered to turn a read-back into UTC.
static bool    dt_legacy;
static int64_t dt_last_utc_ms;
static int16_t dt_last_off;
static bool    dt_last_dst;

static void gp_send_datetime_legacy(void)
{
    time_t local = (time_t)(dt_last_utc_ms / 1000) + dt_last_off * 60;
    struct tm tm; gmtime_r(&local, &tm);
    int y = tm.tm_year + 1900;
    uint8_t b[9] = { 0x0D, 0x07, (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(tm.tm_mon + 1),
                     (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec };
    wr(h_cmd, b, sizeof(b));
    applog_note("gopro set clock (legacy 0x0D) %04d-%02d-%02d %02d:%02d:%02d", y, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static esp_err_t gp_set_datetime(int64_t utc_ms, int16_t off_min, bool dst)
{
    dt_last_utc_ms = utc_ms; dt_last_off = off_min; dt_last_dst = dst;
    if (dt_legacy) { gp_send_datetime_legacy(); return ESP_OK; }
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
    uint8_t b[1] = { dt_legacy ? 0x0E : 0x10 };
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

// Called by cam_task at ~4 Hz (see cam_task driver-poll cadence); every action
// below rate-limits itself off now_ms(), so the call frequency only bounds how
// fine the per-camera poll interval can be.
static esp_err_t gp_tick_1s(void)
{
    uint32_t t0 = now_ms();

    // Not BLE-ready yet: keep probing with Get Hardware Info and send nothing
    // else, until the camera answers or we run out of patience.
    if (!ble_ready) {
        if (t0 - hwinfo_first_ms >= HWINFO_GIVEUP_MS) {
            applog_note("gopro: no hardware-info reply in %u ms -- proceeding anyway",
                        (unsigned)HWINFO_GIVEUP_MS);
            ble_ready = true;
            last_hwinfo_ms = t0;
            gp_pull_all();
            caps_request(300);
            return ESP_OK;
        }
        if (t0 - last_hwinfo_ms >= HWINFO_RETRY_MS) {
            last_hwinfo_ms = t0;
            uint8_t hw[1] = { 0x3C };
            wr(h_cmd, hw, 1);
        }
        return ESP_OK;
    }

    // Gave up waiting earlier: keep asking, slowly. A late success re-registers (parse_cmd_rsp).
    if (!hw_ok && t0 - last_hwinfo_ms >= HWINFO_LATE_RETRY_MS) {
        last_hwinfo_ms = t0;
        uint8_t hw[1] = { 0x3C };
        wr(h_cmd, hw, 1);
    }
    if (q12_n && t0 - q12_t_ms > 4000) { q12_n = 0; }           // an unanswered 0x12 must not shift the matching

    // Reassembly drops mean notifications are being lost or are bigger than our
    // buffer -- either way the OSD silently goes stale, so say it once per event.
    uint16_t drops = (uint16_t)(rz_qry.drops + rz_cmd.drops + rz_set.drops);
    if (drops != last_reasm_drops) {
        last_reasm_drops = drops;
        applog_note("gopro: %u dropped BLE message(s)", drops);
    }

    // keepalive every 3 s (spec 6.4: must be < 10 s).
    if (t0 - last_keepalive_ms >= 3000) {
        last_keepalive_ms = t0;
        uint8_t b[3] = { 0x5B, 0x01, 0x42 };
        wr(h_set, b, 3);
    }

    // Polling fallback. Pushes previously never arrived at all, which looked
    // like the camera ignoring our 0x52/0x53 registration; the real cause was
    // those registrations being dropped before they were sent (see
    // gp_pull_all). With that fixed the pushes should do the work, but the
    // poll stays until pushes are confirmed on real hardware -- it is cheap
    // and a stale OSD is the one failure we care about here. Poll:
    //   STATUSES  (0x13 + the 8 ids we use)          -- per-camera rate
    //   SETTINGS  (0x12 + the 16 zone-pushable ids)   -- 3x that, min 3 s
    // Targeted lists, not get-all: a get-all reply is 25-50 notifications every poll; the
    // targeted ones are ~3 packets. If the camera ever refuses a targeted request we fall
    // back to get-all (poll_full) -- a stale OSD is the one failure that matters here.
    uint32_t t = t0;
    uint32_t status_iv = cfg_active_poll_ms();          // clamped [0.5s, 5s]
    uint32_t setting_iv = status_iv * 3 < 3000 ? 3000 : status_iv * 3;
    if (t - last_status_poll_ms >= status_iv) {
        last_status_poll_ms = t;
        uint8_t b[1 + sizeof(k_st_ids)];
        b[0] = 0x13;
        uint16_t n = 1;
        if (!poll_full) { memcpy(b + 1, k_st_ids, sizeof(k_st_ids)); n += sizeof(k_st_ids); }
        wr(h_qry, b, n);
    }
    if (t - last_getall_ms >= setting_iv) {
        last_getall_ms = t;
        uint8_t b[1 + GP_PSET_MAX];
        b[0] = 0x12;
        uint16_t n = 1;
        if (!poll_full) { for (int i = 0; i < GP_PSET_MAX; i++) { b[n++] = gp_pset_id(i); } }
        wr(h_qry, b, n);
        q12_push(Q12_POLL);
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
    gopro_reasm_init(&rz_cmd, rzb_cmd, sizeof(rzb_cmd));
    gopro_reasm_init(&rz_set, rzb_set, sizeof(rzb_set));
    gopro_reasm_init(&rz_qry, rzb_qry, sizeof(rzb_qry));
    last_keepalive_ms = now_ms();
    getall_retry = 0;
    last_getall_ms = now_ms();
    last_status_poll_ms = now_ms();
    ble_ready = false;
    hw_ok = false;
    dt_legacy = false;
    aspect108_seen = false;
    poll_full = false; poll_err = 0;
    q12_n = 0; lw_n = 0;
    last_reasm_drops = 0;
    memset(&cp, 0, sizeof(cp));
    memset(&wk, 0, sizeof(wk));

    // Nothing but the readiness probe until the camera answers it (spec: poll
    // Get Hardware Info until status 0). gp_tick_1s retries; its reply kicks
    // off registration and the initial reads.
    last_hwinfo_ms = hwinfo_first_ms = now_ms();
    uint8_t hw[1] = { 0x3C };
    wr(h_cmd, hw, 1);

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
    memset(&ap, 0, sizeof(ap));     // pre-LOST commands are dropped, not replayed (spec 5.1)
    memset(&cp, 0, sizeof(cp));
    memset(&wk, 0, sizeof(wk));
    cam_caps_publish(&wk);          // stale capabilities must not outlive the link
    lw_n = 0; q12_n = 0;
    want_record_ack = false;
    ble_ready = false;      // re-probe 0x3C on the next connection
    hw_ok = false;
}

// Register for async updates and pull every value now. Shared by link-up (once
// the camera reports BLE-ready) and the portal "refresh" button.
//
// A query request takes a LIST of element IDs, so this is two writes, not
// fourteen. That matters: ATT allows one outstanding request at a time, so the
// old one-ID-per-write loop had most of its registrations rejected and
// silently dropped -- which is why camera-side changes never pushed to us and
// the polling fallback below had to exist. (Registering for ALL IDs by sending
// an empty list would also work, but then the camera pushes high-rate statuses
// we don't use, e.g. 13 encoding duration and 49 interval countdown.)
static void gp_pull_all(void)
{
    static const uint8_t reg_set[]  = { SET_RESOLUTION, SET_FPS, SET_LENS };      // on every model
    // Model-dependent and unofficial IDs each get their own request: a camera that rejects
    // one must not take the core registration down with it (HERO12+ has 108/183/184, HERO11
    // has the Protune set, ...).
    static const uint8_t reg_set_h12[] = { SET_ASPECT, SET_BIT_DEPTH, SET_PROFILE };
    static const uint8_t reg_set_pro[] = { SET_ISO_MAX, SET_ISO_MIN, SET_SHUTTER,
                                           SET_PROTUNE, SET_WB, SET_COLOR,
                                           SET_SHARPNESS, SET_EV, SET_10BIT };
    _Static_assert(sizeof(reg_set_pro) >= sizeof(k_st_ids), "size b[] off the longest list");
    _Static_assert(sizeof(reg_set_pro) >= sizeof(reg_set_h12), "size b[] off the longest list");
    uint8_t b[1 + sizeof(reg_set_pro)];

    b[0] = 0x53;                                   // register status updates
    memcpy(b + 1, k_st_ids, sizeof(k_st_ids));
    wr(h_qry, b, 1 + sizeof(k_st_ids));

    b[0] = 0x52;                                   // register setting updates
    memcpy(b + 1, reg_set, sizeof(reg_set));
    wr(h_qry, b, 1 + sizeof(reg_set));
    memcpy(b + 1, reg_set_h12, sizeof(reg_set_h12));
    wr(h_qry, b, 1 + sizeof(reg_set_h12));
    memcpy(b + 1, reg_set_pro, sizeof(reg_set_pro));
    wr(h_qry, b, 1 + sizeof(reg_set_pro));

    // The register replies already carry current values, but ask outright too:
    // it also covers anything we read but didn't register for.
    uint8_t g13[1] = { 0x13 }; wr(h_qry, g13, 1);
    uint8_t g12[1] = { 0x12 }; wr(h_qry, g12, 1); q12_push(Q12_POLL);
}

static esp_err_t gp_refresh(void)
{
    if (!h_qry || !ble_ready) { return ESP_ERR_INVALID_STATE; }
    applog_note("gopro: manual refresh");
    gopro_reasm_reset(&rz_qry);
    last_reasm_drops = 0;
    last_getall_ms = last_status_poll_ms = now_ms();
    q12_n = 0;
    gp_pull_all();
    caps_request(0);
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
    case ST_PRESET_GROUP: {
        // status 96 = preset group: 1000 VIDEO / 1001 PHOTO / 1002 TIMELAPSE.
        // (GoPro has no plain "mode" -- switching to Photo doesn't touch the
        // video-resolution setting, so blank res/fps when not VIDEO.)
        uint32_t g = be(v, n);
        uint8_t code = (g == 1001) ? 1 : (g == 1002) ? 2 : (g == 1000) ? 0 : 0xFF;
        if (s->mode.valid && s->mode.v != code) {
            last_getall_ms = 0;         // re-poll settings for the new mode
            caps_request(500);          // video / photo offer different options
        }
        s->mode.valid = (code != 0xFF);
        s->mode.v = code;
        const char *mn = code == 0 ? "VID" : code == 1 ? "PHOTO" : code == 2 ? "TLAPSE" : "";
        val_str_set(&s->mode_name, mn);
        if (!*mn) { s->mode_name.valid = false; }
        if (code != 0) {
            s->iso_max.valid = s->iso_min.valid = s->shutter.valid = false;
            s->resolution.valid = false;
            s->framerate.valid = false;
            s->aspect.valid = false;
        }
        ESP_LOGI(TAG, "preset group %lu -> %s", (unsigned long)g, mn);
        break;
    }
    default: break;
    }
}

// Unknown option value -> "" -> blank (spec 9.4), never a guess.
static void set_str(val_str_t *d, const char *t)
{
    val_str_set(d, t);
    if (!*d->v) { d->valid = false; }
}

static void apply_setting(cam_state_t *s, uint8_t id, const uint8_t *v, uint8_t n)
{
    // Raw value of every setting a zone can push: verifies an apply, and is what
    // the portal's "capture from camera" reads.
    int ri = gp_pset_index(id);
    if (ri >= 0 && n >= 1) {
        s->gp_set[ri].id = id; s->gp_set[ri].valid = true; s->gp_set[ri].v = v[0];
    }
    switch (id) {
    // Resolution / fps / aspect are the VIDEO preset's; the camera keeps reporting them in
    // photo and timelapse, so blank them there (blank-on-unconfirmed) -- also on every
    // poll, not just on the mode change.
    case SET_RESOLUTION:
        if (s->mode.valid && s->mode.v != 0) { s->resolution.valid = false; break; }
        val_str_set(&s->resolution, gopro_resolution_str(v[0]));
        if (!*s->resolution.v) { s->resolution.valid = false; }
        // Aspect implied by the resolution code -- until setting 108 (a real aspect ratio,
        // HERO12+) has been reported; recomputed on EVERY resolution change so a HERO11
        // going 4K 16:9 -> 4K 8:7 does not keep showing the old ratio.
        if (!aspect108_seen) {
            val_str_set(&s->aspect, gopro_aspect_str(v[0]));
            if (!*s->aspect.v) { s->aspect.valid = false; }
        }
        break;
    case SET_ASPECT:
        aspect108_seen = true;
        if (s->mode.valid && s->mode.v != 0) { s->aspect.valid = false; break; }
        val_str_set(&s->aspect, gopro_aspect108_str(v[0]));
        if (!*s->aspect.v) { s->aspect.valid = false; }
        break;
    case SET_FPS:
        if (s->mode.valid && s->mode.v != 0) { s->framerate.valid = false; break; }
        val_str_set(&s->framerate, gopro_fps_str(v[0]));
        if (!*s->framerate.v) { s->framerate.valid = false; }
        break;
    case SET_LENS:
        val_str_set(&s->lens, gopro_lens_str(v[0]));
        if (!*s->lens.v) { s->lens.valid = false; }
        break;
    case SET_PROFILE:
        val_str_set(&s->profile, gopro_profile_str(v[0]));
        if (!*s->profile.v) { s->profile.valid = false; }
        break;
    case SET_BIT_DEPTH:                                  // 0 = 8-bit, 2 = 10-bit; anything else: unknown
        if (v[0] == 0 || v[0] == 2) { s->bit_depth.valid = true; s->bit_depth.v = (v[0] == 2) ? 10 : 8; }
        else                        { s->bit_depth.valid = false; }
        break;
    case SET_ISO_MAX:
    case SET_ISO_MIN:
    case SET_SHUTTER: {
        // Video-mode IDs: blank them outside VIDEO (blank-on-unconfirmed).
        val_str_t *d = id == SET_ISO_MAX ? &s->iso_max : id == SET_ISO_MIN ? &s->iso_min
                                                                          : &s->shutter;
        const char *t = id == SET_SHUTTER ? gopro_shutter_str(v[0]) : gopro_iso_str(v[0]);
        if (s->mode.valid && s->mode.v != 0) { d->valid = false; break; }
        val_str_set(d, t);
        if (!*d->v) { d->valid = false; }
        break;
    }
    case SET_PROTUNE:    set_str(&s->protune,    gopro_onoff_str(v[0]));     break;
    case SET_WB:         set_str(&s->wb,         gopro_wb_str(v[0]));        break;
    case SET_COLOR:      set_str(&s->color,      gopro_color_str(v[0]));     break;
    case SET_SHARPNESS:  set_str(&s->sharpness,  gopro_sharpness_str(v[0])); break;
    case SET_EV:         set_str(&s->ev,         gopro_ev_str(v[0]));        break;
    case SET_10BIT:      set_str(&s->tune_10bit, gopro_onoff_str(v[0]));     break;
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

// Get Hardware Info (0x3C) response: [id][status] then length-value fields
// in order -- model_number (u32 BE), model_name, deprecated, firmware_version,
// serial_number, ap_ssid, ap_mac_address. We only want the two that identify
// what we are actually talking to; they go in the log, which is where a
// support question starts.
static void parse_hw_info(const uint8_t *p, uint16_t len)
{
    char model[24] = "?", fw[24] = "?";
    uint16_t i = 2;                       // past [id][status]
    for (int field = 0; field < 7 && i < len; field++) {
        uint8_t n = p[i++];
        if (i + n > len) { break; }
        if (field == 1 || field == 3) {   // model_name / firmware_version
            char *dst = (field == 1) ? model : fw;
            size_t cap = (field == 1) ? sizeof(model) : sizeof(fw);
            size_t c = n < cap - 1 ? n : cap - 1;
            memcpy(dst, p + i, c);
            dst[c] = '\0';
        }
        i += n;
    }
    applog_note("gopro ready: %s fw %s", model, fw);
    // Keep the model for the portal (preset builder hides settings it lacks).
    // Camera-supplied text: printable ASCII only.
    if (model[0] && strcmp(model, "?") != 0) {
        cam_state_t *s = cam_scratch();
        size_t i = 0;
        for (; model[i] && i < sizeof(s->model) - 1; i++) {
            s->model[i] = (model[i] >= 0x20 && model[i] < 0x7F) ? model[i] : '?';
        }
        s->model[i] = '\0';
        if (CB && CB->on_state_dirty) { CB->on_state_dirty(); }
    }
}

static void parse_cmd_rsp(const uint8_t *p, uint16_t len)
{
    if (len < 2) { return; }
    uint8_t cmd_id = p[0];
    uint8_t status = p[1];

    if (cmd_id == 0x3C) {
        // The camera answers this with an error until its BLE stack is up.
        if (status != 0) { return; }
        if (!hw_ok) {
            hw_ok = true;
            parse_hw_info(p, len);
            // First success -- normally the BLE-ready signal, but also when it arrives AFTER
            // the 3 s give-up: what was sent to a not-yet-listening camera is lost for the
            // whole session, so (re)register and re-read now.
            ble_ready = true;
            gp_pull_all();
            caps_request(300);
        }
        return;
    }
    if ((cmd_id == 0x0F || cmd_id == 0x10) && status != 0 && !dt_legacy) {
        dt_legacy = true;                               // DST variants refused: older firmware
        applog_note("gopro: 0x%02X refused (status %u) -- using legacy date-time 0x0D/0x0E", cmd_id, status);
        if (cmd_id == 0x0F) { gp_send_datetime_legacy(); } else { uint8_t g[1] = { 0x0E }; wr(h_cmd, g, 1); }
        return;
    }
    if (cmd_id == 0x0E && status == 0 && len >= 10) {
        // GET_DATE_TIME (0x0E): [id][status][len][yyyy BE][mm][dd][hh][mi][ss], local time, no offset.
        int y = (p[3] << 8) | p[4];
        int64_t local_epoch = epoch_from_civil_utc(y, p[5], p[6], p[7], p[8], p[9]);
        cam_state_t *s = cam_scratch();
        s->cam_utc_ms.valid = true;
        s->cam_utc_ms.v = (local_epoch - dt_last_off * 60) * 1000;
        s->cam_clock_read_uptime_ms = now_ms();
        if (CB->on_state_dirty) { CB->on_state_dirty(); }
        return;
    }
    if (cmd_id == 0x10 && status == 0 && len >= 13) {
        // GET_DATE_TIME_DST (0x10) response. Resolved: there IS a
        // response_length byte after [cmd_id][status] -- TLV command responses
        // are length-value sequences, and the Open GoPro Python SDK's parser
        // (DateTimeByteParserBuilder.parse) skips exactly that byte before the
        // year. Reading it as part of the date made every field one byte early,
        // so the year was (len<<8)|year_hi and the UTC offset was garbage.
        //   [id][status][response_length][yyyy BE][mm][dd][hh][mi][ss]
        //   [utc_offset int16 BE][is_dst]
        int y = (p[3] << 8) | p[4];
        int mon = p[5], day = p[6], hh = p[7], mm = p[8], ss = p[9];
        int16_t off = (int16_t)((p[10] << 8) | p[11]);
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
        // Response to our keepalive / any setting write: [id][status]. Only the
        // zone-preset apply cares (the keep-alive's 0x5B never matches it).
        if (gopro_reasm_feed(&rz_set, data, len, &pl, &pll) && pll >= 2) { ap_on_set_rsp(pl[0], pl[1]); }
    } else if (handle == vh_qry_rsp) {
        if (gopro_reasm_feed(&rz_qry, data, len, &pl, &pll) && pll >= 2) {
            uint8_t qid = pl[0];
            if (qid == 0x32 || qid == 0x62 || qid == 0xA2) {    // capabilities, not statuses
                caps_on_tlv(qid, pl + 2, (uint16_t)(pll - 2));
                return;
            }
            // Every TLV query response -- get-all (0x12/0x13), register echo
            // (0x52/0x53) AND async push (0x92/0x93) -- is [id][status][tlvs...]
            // (verified: OpenGoPro tutorial_modules TlvResponse.parse @ 0f963572).
            // Settings live under 0x12/0x52/0x92; statuses under 0x13/0x53/0x93.
            bool is_setting = (qid == 0x12 || qid == 0x52 || qid == 0x92);
            ESP_LOGD(TAG, "query id=%02x st=%02x len=%u (%s)",
                     qid, pl[1], pll, is_setting ? "setting" : "status");
            if (pll > 2) { walk_tuples(pl + 2, pll - 2, !is_setting); }
            // Which request did this reply answer? (see q12_*.) Only OUR zone read-back may
            // end a verify -- a periodic poll reply may predate our last write.
            if (qid == 0x12) {
                if (q12_pop() == Q12_VERIFY) {
                    if (ap.state == AP_VERIFY) { ap_evaluate(); }
                    return;
                }
            }
            // A poll the camera refuses (status != 0) twice in a row: stop sending id lists.
            if ((qid == 0x12 || qid == 0x13) && !poll_full) {
                if (pl[1] != 0) {
                    if (++poll_err >= 2) {
                        poll_full = true;
                        applog_note("gopro: targeted poll refused (status %u) -- using get-all", pl[1]);
                    }
                } else {
                    poll_err = 0;
                }
            }
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
        .set_setting = gp_set_setting,
        .set_datetime = gp_set_datetime,
        .get_datetime = gp_get_datetime,
        .push_gps = gp_push_gps,
        .hilight = gp_hilight,
        .tick_1s = gp_tick_1s,
        .tick_fast = gp_tick_fast,
        .refresh = gp_refresh,
        .on_notify = gp_on_notify,
    };
    return &d;
}
