// cam_task.c -- sole owner of the camera BLE link (spec 3.3 / 5.2).
//
// Drives the common lifecycle state machine. NimBLE host callbacks only enqueue
// onto an internal queue that this task drains, so all link mutation is
// single-threaded here. Non-blocking throughout: no call ever waits on a BLE
// response (spec 1.2.4).
#include "camera.h"
#include "ble_central.h"

#include <string.h>

#include "cfg.h"
#include "monoclock.h"
#include "applog.h"
#include "platform.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"

static const char *TAG = "cam";

// ---- timings (spec 5.2 / 8.5) ----------------------------------------
#define CAM_RX_TIMEOUT_MS     5000
#define CONNECT_TIMEOUT_MS   10000
#define TICK_PERIOD_MS        1000
#define LIVENESS_STALL_MS    30000     // housekeeping-style check, spec 3.5

static const uint32_t k_backoff_s[] = { 1, 2, 4, 8, 16, 30, 30 };

// ---- internal event queue -----------------------------------------
typedef enum { IQ_BLE_EVT, IQ_NOTIFY_WAKE, IQ_ADV, IQ_CMD_PAIR, IQ_CMD_FORGET,
               IQ_CMD_SUSPEND, IQ_CMD_SCAN_PAUSE } iq_kind_t;
typedef struct {
    uint8_t kind;
    union {
        struct { int ev; int status; } ble;
        ble_adv_t adv;                    // full advert; cam_task does the matching
        struct { char brand[6]; uint8_t addr[6]; uint8_t has_addr; } pair;
        struct { uint8_t addr[6]; uint8_t has_addr; } forget;   // has_addr=0 -> forget all
        struct { uint8_t on; } flag;
    };
} iq_item_t;

// s_iq carries BLE events + adverts + portal cmds. Inbound GATT notifications do
// NOT go through it: a GoPro get-all-statuses / get-all-settings reply is 25-50
// back-to-back 20-byte notifications, which shared this queue with adverts and
// events and got dropped when it filled (review H14). They have their own byte
// buffer below; s_iq only carries a one-slot "notifications waiting" wake-up.
#define IQ_DEPTH 24
static StaticQueue_t s_iq_s, s_cmdq_s, s_evq_s;
static uint8_t s_iq_buf[IQ_DEPTH * sizeof(iq_item_t)];
static uint8_t s_cmdq_buf[16 * sizeof(cam_cmd_t)];
static uint8_t s_evq_buf[16 * sizeof(cam_event_t)];
static QueueHandle_t s_iq, s_cmdq, s_evq;

// ---- inbound notification buffer ------------------------------------
// Single producer (NimBLE host task, h_notify) -> single consumer (cam_task).
// Message = [handle u16 LE][payload]; the buffer adds its own 4-byte length
// header per message. ~4 KB holds well over a full get-all burst.
#define NB_SIZE        4096
#define NB_PAYLOAD_MAX 256
static StaticMessageBuffer_t s_nb_s;
static uint8_t s_nb_buf[NB_SIZE + 1];
static MessageBufferHandle_t s_nb;
static volatile bool s_nb_wake_pending;     // a wake item is already in s_iq

// ---- driver + link bookkeeping --------------------------------------
static const camera_driver_t *s_drv;
static bool     s_first_pair;
static uint8_t  s_target_addr[6];
static uint8_t  s_target_addr_type;
static int      s_backoff_idx;
static uint32_t s_state_since_ms;

// ---- multi-camera reconnect (spec 7.1) -----------------------------
// Up to CAM_MAX_BONDS cameras can be bonded. On a cold reconnect the scan
// matches ANY bonded address; among the bonded cameras seen within a short
// window it connects to the strongest signal. Once a link is up the scan
// stops (only one camera connected at a time). s_bonds is a snapshot of the
// cfg bond list, refreshed at boot and after every pair/forget.
static cam_bond_t s_bonds[CAM_MAX_BONDS];
static int        s_bond_n;
static bool       s_recon_have;          // a bonded camera has been seen this scan
static uint8_t    s_recon_addr[6];
static uint8_t    s_recon_addr_type;
static int8_t     s_recon_rssi;
static const camera_driver_t *s_recon_drv;
static uint32_t   s_recon_since_ms;      // when the first candidate appeared (0 = none)
#define RECON_PICK_MS 2500               // gather candidates this long before choosing

static void load_bonds(void)
{
    s_bond_n = 0;
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        cam_bond_t b;
        if (cfg_cam_at(i, &b, NULL)) { s_bonds[s_bond_n++] = b; }
    }
}

static int bond_match(const uint8_t addr[6])
{
    for (int i = 0; i < s_bond_n; i++) {
        if (memcmp(s_bonds[i].addr, addr, 6) == 0) { return i; }
    }
    return -1;
}
// spec 3.5 / FMEA row 3: "camera stack hang (connected, silent)" -- the link
// reaches READY, then the camera falls silent and we hit the RX timeout,
// repeatedly. Count ONLY that failure mode (not RF dropouts / power-off, which
// present as a GATT disconnect and are rated impact-none). 3 such failures,
// each following a READY, clustered within 120 s => reboot (fast, clears any
// partial state). The counter is cleared once a READY link has survived 30 s.
static int      s_hang_fails;
static uint32_t s_first_hang_ms;
static bool     s_was_ready;      // link reached READY since the last go_lost
// Consecutive pairing/encryption failures on a *bonded reconnect*. If the camera
// forgot us (re-paired to a phone, "reset connections" on the GoPro), the stored
// bond is dead and every retry fails identically -- so after a few, erase the
// bond and raise FLAG_REPAIR_NEEDED instead of hammering forever.
static int      s_bond_fails;
// Portal scan-pause exemption: set when the link reaches READY, re-armed at the
// start of each portal session (see cam_ble_pause_scan / scan_wanted / apply_scan).
static bool     s_connected_this_session;
// Pairing discovery: collect brand-matching adverts into a list for the portal
// instead of bonding to the first one. Cleared when the user picks one.
#define FOUND_MAX 6
static bool        s_pair_discover;      // in SCAN: list, don't connect
static bool        s_pair_target_set;    // s_target_addr is a user-chosen camera
static uint32_t    s_discover_until;     // stop the discovery scan at this uptime
static cam_found_t s_found[FOUND_MAX];
static int         s_found_n;
static SemaphoreHandle_t s_found_mux;
static StaticSemaphore_t s_found_mux_buf;
// The one shared camera-state scratch (spec 3.4). cam_task + the driver
// callbacks it invokes all mutate *cam_scratch()*; cam_state_commit() publishes.
#define s_cs (*cam_scratch())

// ---- helpers ----------------------------------------------------
static void set_link(cam_link_state_t st)
{
    if (s_cs.link == st) { return; }
    s_cs.link = st;
    s_state_since_ms = now_ms();
    if (st == CAM_LINK_READY) { s_was_ready = true; s_connected_this_session = true; }
    cam_state_commit();
    cam_event_t e = { .type = CAM_EV_LINK, .value = (int32_t)st };
    xQueueSend(s_evq, &e, 0);
    ESP_LOGI(TAG, "link -> %d", st);
}

static void publish_dirty(void)
{
    cam_state_commit();
    cam_event_t e = { .type = CAM_EV_STATE_DIRTY };
    xQueueSend(s_evq, &e, 0);
}

static void invalidate_cam_values(void)   // spec 8.5 step 1
{
    s_cs.recording = TRIBOOL_UNKNOWN;
    s_cs.pre_record = TRIBOOL_UNKNOWN;
    s_cs.busy = TRIBOOL_UNKNOWN;
    s_cs.battery_pct.valid = false;
    s_cs.sd_remaining_kb.valid = false;
    s_cs.rec_elapsed_s.valid = false;
    s_cs.rec_remaining_s.valid = false;
    s_cs.resolution.valid = false;
    s_cs.framerate.valid = false;
    s_cs.aspect.valid = false;
    s_cs.profile.valid = false;
    s_cs.bit_depth.valid = false;
    s_cs.iso_max.valid = s_cs.iso_min.valid = s_cs.shutter.valid = false;
    s_cs.protune.valid = s_cs.wb.valid = s_cs.color.valid = false;
    s_cs.sharpness.valid = s_cs.ev.valid = s_cs.tune_10bit.valid = false;
    s_cs.lens.valid = false;
    s_cs.mode.valid = false;
    s_cs.mode_name.valid = false;
    s_cs.cam_utc_ms.valid = false;
    for (int i = 0; i < GP_PSET_MAX; i++) { s_cs.gp_set[i].valid = false; }
    s_cs.model[0] = '\0';
    s_cs.gp_apply_zone = 0;
    s_cs.gp_apply_state = GPA_NONE;
    s_cs.gp_apply_fail = 0;
    s_cs.sd_missing = s_cs.sd_full = s_cs.hot = false;
}

// ---- driver-facing callbacks (run in cam_task ctx) ----------------
static void cb_link_state(cam_link_state_t s) { set_link(s); }
static void cb_state_dirty(void) { publish_dirty(); }
static void cb_ack(cam_op_t op, bool ok)
{
    cam_event_t e = { .type = CAM_EV_ACK, .value = ((int32_t)op << 16) | (ok ? 1 : 0) };
    xQueueSend(s_evq, &e, 0);
}
static void cb_paired(const uint8_t addr[6], const char *name, uint32_t dji_id)
{
    // spec 6.1 / 7.3 / 11.0: persist synchronously BEFORE we report READY.
    esp_err_t pe = cfg_set_camera_bond(s_drv->name, addr, name, dji_id);
    if (pe == ESP_ERR_NO_MEM) {
        applog_note("camera bond list full (%d) -- forget one before pairing another", CAM_MAX_BONDS);
    }
    load_bonds();
    memcpy(s_target_addr, addr, 6);
    cam_event_t e = { .type = CAM_EV_PAIRED };
    xQueueSend(s_evq, &e, 0);
    applog_note("camera paired: %s %s", s_drv->name, name ? name : "");
}
static const camera_cbs_t k_cbs = {
    .on_link_state = cb_link_state,
    .on_state_dirty = cb_state_dirty,
    .on_ack = cb_ack,
    .on_paired = cb_paired,
};
void cam_registry_set_cbs(const camera_cbs_t *c);   // cam_registry.c

// ---- NimBLE host-task callbacks: ONLY enqueue -----------------------
// These run on the NimBLE host task. They must not read cam_task's private
// state (s_drv / s_target_addr / s_first_pair) -- that is a data race and a
// NULL-deref reboot when a portal "forget" nulls s_drv mid-scan (review H4).
// The advert is forwarded whole; cam_task does all the matching.
static void h_scan(const ble_adv_t *adv)
{
    iq_item_t it = { .kind = IQ_ADV };
    it.adv = *adv;
    xQueueSend(s_iq, &it, 0);
}
static void h_evt(ble_central_evt_t ev, int status)
{
    iq_item_t it = { .kind = IQ_BLE_EVT, .ble = { .ev = ev, .status = status } };
    xQueueSend(s_iq, &it, 0);
}
static volatile uint32_t s_notify_drops;
static volatile uint32_t s_notify_drops_total;
uint32_t cam_notify_drops(void) { return s_notify_drops_total; }
static void h_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    uint8_t msg[2 + NB_PAYLOAD_MAX];
    if (len > NB_PAYLOAD_MAX) { len = NB_PAYLOAD_MAX; }
    msg[0] = (uint8_t)handle;
    msg[1] = (uint8_t)(handle >> 8);
    memcpy(msg + 2, data, len);
    if (xMessageBufferSend(s_nb, msg, 2u + len, 0) == 0) {
        s_notify_drops++;          // surfaced in the 1 Hz tick log
        s_notify_drops_total++;    // monotonic, surfaced in /api/status
        return;
    }
    // One wake-up in s_iq at a time, however many notifications arrive. If it
    // can't be queued, cam_task still drains the buffer on its next pass.
    if (!s_nb_wake_pending) {
        s_nb_wake_pending = true;
        iq_item_t it = { .kind = IQ_NOTIFY_WAKE };
        if (xQueueSend(s_iq, &it, 0) != pdTRUE) { s_nb_wake_pending = false; }
    }
}

// cam_task ctx: hand every buffered notification to the driver.
static void drain_notifications(void)
{
    static uint8_t msg[2 + NB_PAYLOAD_MAX];
    s_nb_wake_pending = false;      // clear BEFORE draining so a late arrival re-wakes us
    size_t n;
    while ((n = xMessageBufferReceive(s_nb, msg, sizeof(msg), 0)) >= 2) {
        s_cs.last_rx_ms = now_ms();
        if (s_drv && s_drv->on_notify) {
            s_drv->on_notify((uint16_t)(msg[0] | (msg[1] << 8)), msg + 2, (uint16_t)(n - 2));
        }
    }
}

// ---- public submit API ------------------------------------------
esp_err_t cam_submit(cam_op_t op, int32_t arg)
{
    cam_cmd_t c = { .op = (uint8_t)op, .arg = arg };
    // drop-duplicate (spec 3.3): scan the queue for the same op.
    UBaseType_t n = uxQueueMessagesWaiting(s_cmdq);
    for (UBaseType_t i = 0; i < n; i++) {
        cam_cmd_t peek;
        if (xQueueReceive(s_cmdq, &peek, 0) != pdTRUE) { break; }
        if (!(peek.op == c.op && peek.arg == c.arg)) { xQueueSend(s_cmdq, &peek, 0); }
    }
    if (xQueueSend(s_cmdq, &c, 0) != pdTRUE) {
        cam_cmd_t drop;
        xQueueReceive(s_cmdq, &drop, 0);
        xQueueSend(s_cmdq, &c, 0);
    }
    return ESP_OK;
}
esp_err_t cam_submit_datetime(int64_t utc_ms, int16_t off, bool dst)
{
    cam_cmd_t c = { .op = CAM_OP_SET_DATETIME, .arg = ((int32_t)off << 8) | (dst ? 1 : 0), .arg64 = utc_ms };
    xQueueSend(s_cmdq, &c, 0);
    return ESP_OK;
}
static gps_fix_t s_gps_for_push;   // latest fix to accompany CAM_OP_PUSH_GPS
esp_err_t cam_submit_gps(const gps_fix_t *fix, int64_t utc_ms)
{
    if (fix) { s_gps_for_push = *fix; }
    cam_cmd_t c = { .op = CAM_OP_PUSH_GPS, .arg64 = utc_ms };
    xQueueSend(s_cmdq, &c, 0);
    return ESP_OK;
}
esp_err_t cam_pair_begin(const char *brand)
{
    iq_item_t it = { .kind = IQ_CMD_PAIR };
    strlcpy(it.pair.brand, brand, sizeof(it.pair.brand));
    return xQueueSend(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
}
esp_err_t cam_pair_select(const char *brand, const uint8_t addr[6])
{
    iq_item_t it = { .kind = IQ_CMD_PAIR, .pair = { .has_addr = 1 } };
    strlcpy(it.pair.brand, brand, sizeof(it.pair.brand));
    memcpy(it.pair.addr, addr, 6);
    return xQueueSend(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
}
int cam_found_list(cam_found_t *out, int max)
{
    if (!s_found_mux || !(s_pair_discover || s_pair_target_set)) { return 0; }
    xSemaphoreTake(s_found_mux, portMAX_DELAY);
    int n = s_found_n < max ? s_found_n : max;
    for (int i = 0; i < n; i++) { out[i] = s_found[i]; }
    xSemaphoreGive(s_found_mux);
    return n;
}
// cam_task ctx: add/refresh a discovered advert (best RSSI wins).
static void found_add(const ble_adv_t *a)
{
    xSemaphoreTake(s_found_mux, portMAX_DELAY);
    for (int i = 0; i < s_found_n; i++) {
        if (memcmp(s_found[i].addr, a->addr, 6) == 0) {
            if (a->rssi > s_found[i].rssi) { s_found[i].rssi = a->rssi; }
            if (a->name[0]) { strlcpy(s_found[i].name, a->name, sizeof(s_found[i].name)); }
            xSemaphoreGive(s_found_mux);
            return;
        }
    }
    if (s_found_n < FOUND_MAX) {
        cam_found_t *f = &s_found[s_found_n++];
        memcpy(f->addr, a->addr, 6);
        f->addr_type = a->addr_type;
        f->rssi = a->rssi;
        strlcpy(f->name, a->name[0] ? a->name : "GoPro", sizeof(f->name));
    }
    xSemaphoreGive(s_found_mux);
}
esp_err_t cam_forget(void)
{
    iq_item_t it = { .kind = IQ_CMD_FORGET };   // has_addr = 0 -> forget all
    return xQueueSend(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
}
esp_err_t cam_forget_one(const uint8_t addr[6])
{
    iq_item_t it = { .kind = IQ_CMD_FORGET, .forget = { .has_addr = 1 } };
    memcpy(it.forget.addr, addr, 6);
    return xQueueSend(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
}
esp_err_t cam_refresh(void) { return cam_submit(CAM_OP_REFRESH, 0); }
void cam_ble_suspend(void)
{
    iq_item_t it = { .kind = IQ_CMD_SUSPEND };
    xQueueSend(s_iq, &it, pdMS_TO_TICKS(100));
}
static volatile bool s_throttle;
void cam_throttle(bool on) { s_throttle = on; }

// Portal Wi-Fi AP is up: on the single-radio C3 the aggressive active BLE scan
// starves the macOS association handshake (auth/assoc/EAPOL/DHCP) and HTTP. See
// scan_wanted() for the policy -- a *cold* pairing hunt is fully paused, a
// *bonded reconnect* drops to a gentle low-duty passive scan (Wi-Fi-friendly,
// runs continuously) since the portal is up at every disarmed boot, and an
// in-progress pairing or an already-live session keeps the aggressive scan.
static volatile bool s_scan_paused;
void cam_ble_pause_scan(bool on)
{
    iq_item_t it = { .kind = IQ_CMD_SCAN_PAUSE, .flag = { .on = on ? 1 : 0 } };
    xQueueSend(s_iq, &it, pdMS_TO_TICKS(100));
}
QueueHandle_t cam_events(void) { return s_evq; }

void cam_erase_all_bonds(void) { ble_central_erase_all_bonds(); }

// Boot-time reconcile of cfg.camera vs the NimBLE bond store (spec 11.0 /
// 3.6 step 1). Returns true if FLAG_REPAIR_NEEDED should be raised.
bool cam_reconcile_bonds(bool erase_orphan_bond)
{
    // Wait for the NimBLE host: bond-store queries before sync are meaningless,
    // and acting on a wrong count either strands the camera (REPAIR_NEEDED) or
    // erases a valid bond (review H15).
    for (int i = 0; i < 200 && !ble_central_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ble_central_synced()) {
        applog_note("BLE host not synced -- skipping bond reconcile (no changes)");
        return false;
    }

    if (erase_orphan_bond) {
        ble_central_erase_all_bonds();
        return false;
    }
    int cams  = cfg_cam_count();
    int bonds = ble_central_bond_count();
    if (cams > 0 && bonds == 0) {
        applog_note("cfg has %d camera(s) but no BLE bond -> REPAIR_NEEDED", cams);
        return true;                          // caller sets flag; cam stays IDLE-ish
    }
    if (cams == 0 && bonds > 0) {
        applog_note("orphan BLE bond(s), no cfg camera -> erasing");
        ble_central_erase_all_bonds();
    }
    // A partial mismatch (cfg 2, bonds 1) is left alone: the camera with a live
    // NimBLE bond still reconnects; one without hits the stale-bond path and
    // self-heals into FLAG_REPAIR_NEEDED.
    return false;
}

// ---- command execution ----------------------------------------
static void exec_cmd(const cam_cmd_t *c)
{
    if (!s_drv || s_cs.link != CAM_LINK_READY) {
        // spec 5.2 / 8.1: drop commands generated before/outside READY.
        ESP_LOGD(TAG, "drop cmd %u (link %d)", c->op, s_cs.link);
        return;
    }
    switch ((cam_op_t)c->op) {
    case CAM_OP_RECORD_START: s_drv->record(true); break;
    case CAM_OP_RECORD_STOP:  s_drv->record(false); break;
    case CAM_OP_SELECT_PRESET: s_drv->select_preset((uint8_t)c->arg); break;
    case CAM_OP_SET_DATETIME:
        s_drv->set_datetime(c->arg64, (int16_t)(c->arg >> 8), c->arg & 1);
        break;
    case CAM_OP_GET_DATETIME: s_drv->get_datetime(); break;
    case CAM_OP_PUSH_GPS:
        s_drv->push_gps(&s_gps_for_push, c->arg64);
        break;
    case CAM_OP_HILIGHT: s_drv->hilight(); break;
    case CAM_OP_REFRESH:
        if (s_drv->refresh) { s_drv->refresh(); }
        break;
    case CAM_OP_SET_SETTING:
        if (s_drv->set_setting) { s_drv->set_setting((uint8_t)(c->arg >> 8), (uint8_t)c->arg); }
        break;
    }
}

// ---- state machine step --------------------------------------
// Portal AP up (cam_ble_pause_scan): the aggressive active scan starves Wi-Fi
// association / HTTP on the single-radio C3. Policy while the AP is up:
//   - in-progress pairing (s_first_pair / s_pair_discover) -> aggressive active
//     scan (needs the scan response for the camera name; user tolerates a hiccup)
//   - already-live session (s_connected_this_session) -> aggressive
//   - bonded reconnect -> GENTLE: low-duty passive scan (~11% radio, no TX),
//     which coexists with Wi-Fi. Runs continuously -- no duty cycle -- so a
//     paired camera reconnects even though the portal is up at every disarmed
//     boot.
//   - nothing bonded -> scan off (cold hunt only makes sense from the portal).

// Should the BLE scanner be active right now, and if so, gentle?
static bool scan_wanted(bool *gentle)
{
    bool aggressive_ok = !s_scan_paused || s_first_pair || s_pair_discover ||
                         s_connected_this_session;
    if (aggressive_ok) { if (gentle) { *gentle = false; } return true; }
    if (s_bond_n > 0)  { if (gentle) { *gentle = true;  } return true; }
    if (gentle) { *gentle = false; }
    return false;
}

static const char *s_scan_state = "off";   // portal diagnostics: off / active / gentle

static void apply_scan(void)
{
    bool gentle = false;
    bool want = scan_wanted(&gentle);
    ble_central_scan_gentle(gentle);
    ble_central_scan(want);
    s_scan_state = !want ? "off" : gentle ? "gentle" : "active";
}

// portal BLE diagnostics (Status tab).
uint32_t cam_link_age_ms(void)   { return now_ms() - s_state_since_ms; }
const char *cam_scan_state(void) { return s_scan_state; }

static void begin_scan(void)
{
    invalidate_cam_values();
    s_recon_have = false;
    s_recon_since_ms = 0;
    set_link(CAM_LINK_SCAN);
    apply_scan();
}

// Lock onto one advertiser and start connecting. `d` is the driver for that
// camera's brand. On a reconnect (not first_pair) this also records which
// bonded camera is now active so ctrl/osd/driver code sees the right brand.
static void connect_now(const uint8_t addr[6], uint8_t addr_type,
                        const char *name, const camera_driver_t *d)
{
    memcpy(s_target_addr, addr, 6);
    s_target_addr_type = addr_type;
    memcpy(s_cs.addr, addr, 6);
    if (name && name[0]) { strlcpy(s_cs.name, name, sizeof(s_cs.name)); }
    s_drv = d;
    if (!s_first_pair) { cfg_set_active_camera(addr); }   // sync commit, rare
    ble_central_scan(false);
    set_link(CAM_LINK_CONNECTING);
    ble_central_connect(s_target_addr, s_target_addr_type);
}

// `silent_hang` = the loss was an RX-silence timeout on an established link
// (FMEA row 3), as opposed to a disconnect / bond / discovery failure.
static void go_lost(const char *why, bool silent_hang)
{
    applog_note("cam LOST: %s", why);
    rtcnv_set_last_disconnect(why);
    ble_central_disconnect();
    if (s_drv && s_drv->link_down) { s_drv->link_down(); }
    invalidate_cam_values();
    set_link(CAM_LINK_LOST);
    // flush pending commands (spec 8.5 step 2)
    cam_cmd_t drop;
    while (xQueueReceive(s_cmdq, &drop, 0) == pdTRUE) { }
    cfg_stats_bump_reconnect();

    // spec 3.5 / FMEA row 3: reboot only on a repeating connect-then-silent
    // pattern. A GATT disconnect (camera off, RF dropout) is not counted.
    if (s_was_ready && silent_hang) {
        uint32_t now = now_ms();
        if (s_hang_fails == 0 || (now - s_first_hang_ms) > 120000) {
            s_hang_fails = 0;
            s_first_hang_ms = now;
        }
        if (++s_hang_fails >= 3) {
            applog_note("cam silent-hang x%d in %lus -> reboot",
                        s_hang_fails, (unsigned long)((now - s_first_hang_ms) / 1000));
            vTaskDelay(pdMS_TO_TICKS(50));   // let the log flush
            esp_restart();
        }
    }
    s_was_ready = false;
}

static void handle_ble_evt(int ev, int status)
{
    // A queued BLE event can arrive after a portal "forget" nulled s_drv
    // (both drain from s_iq). Everything past CONNECTED needs the driver.
    if (!s_drv && ev != BLE_EVT_DISCONNECTED) { return; }

    switch (ev) {
    case BLE_EVT_CONNECTED:
        if (s_cs.link == CAM_LINK_CONNECTING) {
            ESP_LOGI(TAG, "connected, securing");
            ble_central_secure();
        }
        break;
    case BLE_EVT_ENC_DONE:
        s_bond_fails = 0;
        set_link(CAM_LINK_SYNCING);
        ble_central_discover_all();
        break;
    case BLE_EVT_ENC_FAIL:
        // On a bonded reconnect, repeated identical pairing failures mean the
        // camera dropped its side of the bond -- erasing ours and asking for a
        // re-pair is the only real fix (spec 3.6 / 11.0 FLAG_REPAIR_NEEDED).
        if (!s_first_pair && ++s_bond_fails >= 3) {
            applog_note("cam: pairing failed x%d -- bond stale, erasing + REPAIR_NEEDED", s_bond_fails);
            ble_central_erase_all_bonds();
            bridge_flag_set(FLAG_REPAIR_NEEDED);
            s_bond_fails = 0;
            go_lost("bond stale (repair needed)", false);
            s_drv = NULL;                 // stop reconnecting until re-paired
            set_link(CAM_LINK_IDLE);
            break;
        }
        go_lost("bond failed", false);
        break;
    case BLE_EVT_MTU_DONE:
        ESP_LOGI(TAG, "MTU = %d", status);
        break;
    case BLE_EVT_DISC_DONE: {
        // subscribe the brand's notify chars, then hand to the driver.
        const brand_gatt_t *g = s_drv->gatt ? s_drv->gatt() : NULL;
        if (g) {
            for (int i = 0; i < g->sub_n; i++) {
                uint16_t h = ble_central_val_handle(g->sub[i]);
                if (h) { ble_central_subscribe(h, true); }
                else { ESP_LOGW(TAG, "sub char %d not found", i); }
            }
        }
        esp_err_t r = s_drv->link_up ? s_drv->link_up(s_first_pair) : ESP_OK;
        if (r != ESP_OK) { go_lost("driver link_up failed", false); break; }
        // Driver must call CB->on_link_state(CAM_LINK_READY) once its protocol
        // sync completes (GoPro get-all returns / DUML handshake acks). The
        // CONNECT_TIMEOUT_MS guard covers a driver that never gets there.
        s_backoff_idx = 0;
        s_cs.last_rx_ms = now_ms();
        if (s_first_pair) {
            // driver's on_paired persisted cfg; clear the pairing flags
            s_first_pair = false;
        }
        s_pair_discover = s_pair_target_set = false;
        break;
    }
    case BLE_EVT_DISC_FAIL:
        go_lost("gatt discovery failed", false);
        break;
    case BLE_EVT_DISCONNECTED:
        if (s_cs.link != CAM_LINK_IDLE) { go_lost("gatt disconnect", false); }
        break;
    }
}

static void cam_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_CAM);
    ble_central_set_cbs(h_scan, h_evt, h_notify);

    const cfg_t *cfg = cfg_get();
    load_bonds();
    if (bridge_flag_get(FLAG_REPAIR_NEEDED)) {
        ESP_LOGW(TAG, "REPAIR_NEEDED: staying IDLE until re-pair");
        set_link(CAM_LINK_IDLE);
    } else if (s_bond_n > 0) {
        // Provisional driver: the reconnect scan picks the actual camera (and
        // its driver) by RSSI. Prefer the last-active brand for slot 0.
        const char *b0 = (cfg && cfg->camera.bonded && cfg->camera.brand[0])
                         ? cfg->camera.brand : s_bonds[0].brand;
        s_drv = camera_by_name(b0);
        if (!s_drv) { s_drv = camera_by_name(s_bonds[0].brand); }
        if (s_drv) {
            memcpy(s_target_addr, s_bonds[0].addr, 6);   // provisional
            s_target_addr_type = 0;   // public; VERIFY per camera (GoPro random?)
            s_first_pair = false;
            ESP_LOGI(TAG, "boot: %d bonded camera(s) -> reconnect scan", s_bond_n);
            begin_scan();
        }
    } else {
        set_link(CAM_LINK_IDLE);      // pairing only via portal (spec 3.6 step 5)
    }

    uint32_t last_tick = now_ms();
    uint32_t last_drv_poll = now_ms();

    for (;;) {
        iq_item_t it;
        drain_notifications();      // before blocking: a wake-up in s_iq ends the wait below
        if (xQueueReceive(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (it.kind) {
            case IQ_ADV: {
                if (s_cs.link != CAM_LINK_SCAN || !s_drv) { break; }
                static uint32_t adv_seen, adv_log_ms;
                adv_seen++;
                if (now_ms() - adv_log_ms > 3000) {
                    adv_log_ms = now_ms();
                    ESP_LOGI(TAG, "scanning (%s): %lu adverts seen; last %02x:%02x:%02x:%02x:%02x:%02x '%s' svc=%04x mfg[0..1]=%02x%02x",
                             s_drv->name, (unsigned long)adv_seen,
                             it.adv.addr[5], it.adv.addr[4], it.adv.addr[3],
                             it.adv.addr[2], it.adv.addr[1], it.adv.addr[0],
                             it.adv.name, it.adv.svc_uuid16,
                             it.adv.mfg_len ? it.adv.mfg[0] : 0,
                             it.adv.mfg_len > 1 ? it.adv.mfg[1] : 0);
                }
                if (s_pair_discover) {
                    if (s_drv->adv_match && s_drv->adv_match(&it.adv)) { found_add(&it.adv); }
                    break;                       // never connect while listing
                }

                if (s_first_pair && !s_pair_target_set) {
                    // Brand-match pairing (DJI): connect to the first match.
                    if (s_drv->adv_match && s_drv->adv_match(&it.adv)) {
                        ESP_LOGI(TAG, "match: %s '%s' -> connecting", s_drv->name, it.adv.name);
                        connect_now(it.adv.addr, it.adv.addr_type, it.adv.name, s_drv);
                    }
                    break;
                }
                if (s_pair_target_set) {
                    // User picked a specific camera in the discovery list.
                    if (memcmp(it.adv.addr, s_target_addr, 6) == 0) {
                        ESP_LOGI(TAG, "pair target '%s' -> connecting", it.adv.name);
                        connect_now(it.adv.addr, it.adv.addr_type, it.adv.name, s_drv);
                    }
                    break;
                }

                // Reconnect: match any bonded camera, prefer the strongest.
                int bi = bond_match(it.adv.addr);
                if (bi < 0) { break; }
                const camera_driver_t *bd = camera_by_name(s_bonds[bi].brand);
                if (!bd) { break; }
                if (s_bond_n == 1) {
                    ESP_LOGI(TAG, "reconnect '%s' -> connecting", it.adv.name);
                    connect_now(it.adv.addr, it.adv.addr_type,
                                it.adv.name[0] ? it.adv.name : s_bonds[bi].name, bd);
                    break;
                }
                if (!s_recon_have || it.adv.rssi > s_recon_rssi) {
                    s_recon_have = true;
                    memcpy(s_recon_addr, it.adv.addr, 6);
                    s_recon_addr_type = it.adv.addr_type;
                    s_recon_rssi = it.adv.rssi;
                    s_recon_drv = bd;
                    strlcpy(s_cs.name, it.adv.name[0] ? it.adv.name : s_bonds[bi].name,
                            sizeof(s_cs.name));
                }
                if (!s_recon_since_ms) { s_recon_since_ms = now_ms(); }
                break;
            }
            case IQ_BLE_EVT:
                handle_ble_evt(it.ble.ev, it.ble.status);
                break;
            case IQ_NOTIFY_WAKE:
                break;              // drained below, same pass
            case IQ_CMD_PAIR: {
                const camera_driver_t *d = camera_by_name(it.pair.brand);
                if (d && cfg_cam_count() >= CAM_MAX_BONDS &&
                    !(it.pair.has_addr && bond_match(it.pair.addr) >= 0)) {
                    applog_note("pair refused: bond list full (%d)", CAM_MAX_BONDS);
                    d = NULL;
                }
                if (d) {
                    if (ble_central_connected()) { ble_central_disconnect(); }
                    s_drv = d;
                    s_first_pair = true;
                    s_backoff_idx = 0;
                    s_bond_fails = 0;
                    bridge_flag_clear(FLAG_REPAIR_NEEDED);
                    if (it.pair.has_addr) {
                        // User picked a discovered camera -> bond to that address.
                        s_pair_discover = false;
                        s_pair_target_set = true;
                        memcpy(s_target_addr, it.pair.addr, 6);
                        s_target_addr_type = 0;
                        xSemaphoreTake(s_found_mux, portMAX_DELAY);
                        for (int i = 0; i < s_found_n; i++) {
                            if (memcmp(s_found[i].addr, it.pair.addr, 6) == 0) {
                                s_target_addr_type = s_found[i].addr_type; break;
                            }
                        }
                        xSemaphoreGive(s_found_mux);
                        ESP_LOGI(TAG, "pair select: %s %02x:%02x:%02x:%02x:%02x:%02x",
                                 it.pair.brand, it.pair.addr[0], it.pair.addr[1], it.pair.addr[2],
                                 it.pair.addr[3], it.pair.addr[4], it.pair.addr[5]);
                    } else if (strcmp(it.pair.brand, "gopro") == 0) {
                        // Start discovery: list brand matches, don't connect.
                        s_pair_discover = true;
                        s_pair_target_set = false;
                        s_discover_until = now_ms() + 45000;
                        xSemaphoreTake(s_found_mux, portMAX_DELAY);
                        s_found_n = 0;
                        xSemaphoreGive(s_found_mux);
                        ESP_LOGI(TAG, "pair discover: %s", it.pair.brand);
                    } else {
                        // Legacy first-match pairing (DJI).
                        s_pair_discover = false;
                        s_pair_target_set = false;
                        ESP_LOGI(TAG, "pairing: %s", it.pair.brand);
                    }
                    begin_scan();
                }
                break;
            }
            case IQ_CMD_FORGET: {
                bool forget_all = !it.forget.has_addr;
                bool hit_active = forget_all ||
                    memcmp(it.forget.addr, s_target_addr, 6) == 0 ||
                    memcmp(it.forget.addr, s_cs.addr, 6) == 0;

                if (forget_all) {
                    ble_central_disconnect();
                    ble_central_erase_all_bonds();
                    cfg_clear_camera();
                } else {
                    ble_central_forget_bond_any(it.forget.addr);
                    cfg_forget_camera(it.forget.addr);
                }
                load_bonds();
                s_pair_discover = s_pair_target_set = false;

                if (hit_active || s_bond_n == 0) {
                    if (!forget_all) { ble_central_disconnect(); }
                    invalidate_cam_values();
                    if (s_bond_n > 0) {
                        s_drv = camera_by_name(s_bonds[0].brand);
                        s_backoff_idx = 0;
                        memcpy(s_target_addr, s_bonds[0].addr, 6);
                        s_first_pair = false;
                        begin_scan();
                    } else {
                        s_drv = NULL;
                        memset(s_target_addr, 0, 6);
                        set_link(CAM_LINK_IDLE);
                    }
                }
                applog_note(forget_all ? "all cameras forgotten" : "camera forgotten");
                break;
            }
            case IQ_CMD_SUSPEND:
                // OTA in progress: drop BLE so it doesn't contend with Wi-Fi +
                // flash writes on the single core. A reboot restores it.
                ble_central_scan(false);
                ble_central_disconnect();
                if (s_drv && s_drv->link_down) { s_drv->link_down(); }
                s_drv = NULL;                 // don't reconnect
                invalidate_cam_values();
                set_link(CAM_LINK_IDLE);
                applog_note("BLE suspended for OTA");
                break;
            case IQ_CMD_SCAN_PAUSE:
                s_scan_paused = it.flag.on;
                if (s_scan_paused) {
                    // New portal session: a camera connected right now stays on
                    // the aggressive scan; a bonded-but-disconnected camera drops
                    // to the gentle low-duty reconnect scan.
                    s_connected_this_session = (s_cs.link == CAM_LINK_READY);
                    if (s_cs.link == CAM_LINK_SCAN) { apply_scan(); }
                    applog_note(s_bond_n > 0 && !s_connected_this_session
                                ? "portal AP up: reconnect scan -> gentle (Wi-Fi-friendly)"
                                : "portal AP up: BLE hunt paused");
                } else {
                    s_connected_this_session = false;
                    // Portal down: resume promptly if we were hunting.
                    if (s_drv && (s_cs.link == CAM_LINK_SCAN || s_cs.link == CAM_LINK_LOST)) {
                        s_backoff_idx = 0;
                        begin_scan();
                    }
                    applog_note("BLE scan resumed");
                }
                break;
            }
        }

        uint32_t now = now_ms();

        // ---- driver poll: call the driver's tick at the configured per-camera
        // interval so rates down to 0.5 s are reachable. While the portal AP is
        // up the interval is floored at 1.5 s (single-radio coex headroom; the
        // portal page only refreshes that fast anyway). In flight it runs at the
        // configured rate and feeds the OSD directly.
        {
            uint32_t drv_iv = cfg_active_poll_ms();
            if (s_throttle && drv_iv < 1500) { drv_iv = 1500; }
            if (s_cs.link == CAM_LINK_READY && s_drv && s_drv->tick_1s &&
                now - last_drv_poll >= drv_iv) {
                last_drv_poll = now;
                s_drv->tick_1s();
            }
        }

        // ---- periodic tick (1 Hz, or 4 s while throttled for the portal) ----
        if (now - last_tick >= (s_throttle ? 4000 : TICK_PERIOD_MS)) {
            last_tick = now;
            if (s_cs.link == CAM_LINK_READY) {
                s_cs.rssi = ble_central_last_rssi();

                // RX-silence timeout (spec 8.5)
                if (now - s_cs.last_rx_ms > CAM_RX_TIMEOUT_MS) {
                    go_lost("rx timeout 5 s", true);
                } else if (now - s_state_since_ms > 30000) {
                    s_hang_fails = 0;   // a READY link that has held 30 s is healthy
                }
            }

            if (s_notify_drops) {
                ESP_LOGW(TAG, "dropped %lu BLE notifications (queue full)",
                         (unsigned long)s_notify_drops);
                s_notify_drops = 0;
            }

            // Re-arm scanning while in SCAN: covers ble_central_scan() having
            // been rejected because the host wasn't synced yet (review M10), and
            // NimBLE dropping discovery on its own. apply_scan() picks the mode
            // (aggressive / gentle / off) per scan_wanted().
            if (s_pair_discover && (int32_t)(now - s_discover_until) >= 0) {
                ble_central_scan(false);   // discovery window closed; list stays for the portal
            } else if (s_cs.link == CAM_LINK_SCAN && s_drv) {
                apply_scan();
            }

            publish_dirty();
        }

        // ---- multi-camera reconnect: commit to the best candidate ----------
        // Gather bonded-camera adverts for RECON_PICK_MS, then connect to the
        // strongest (or immediately once one is very close).
        if (s_cs.link == CAM_LINK_SCAN && s_recon_have && s_recon_since_ms &&
            (now - s_recon_since_ms >= RECON_PICK_MS || s_recon_rssi >= -50)) {
            ESP_LOGI(TAG, "reconnect: %d cams in range, best %d dBm -> connecting",
                     s_bond_n, s_recon_rssi);
            connect_now(s_recon_addr, s_recon_addr_type, s_cs.name, s_recon_drv);
        }

        // ---- connect / sync timeout -----------------------------
        if ((s_cs.link == CAM_LINK_CONNECTING || s_cs.link == CAM_LINK_SYNCING) &&
            now - s_state_since_ms > CONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "connect/sync timeout -> rescan");
            ble_central_disconnect();
            begin_scan();
        }

        // ---- LOST backoff -> rescan ----------------------------
        // begin_scan()/apply_scan() picks the radio mode (aggressive / gentle /
        // off); the backoff just paces the re-entry.
        if (s_cs.link == CAM_LINK_LOST) {
            uint32_t wait_ms = k_backoff_s[s_backoff_idx] * 1000;
            if (now - s_state_since_ms >= wait_ms) {
                if (s_backoff_idx < (int)(sizeof(k_backoff_s)/sizeof(k_backoff_s[0])) - 1) {
                    s_backoff_idx++;
                }
                begin_scan();
            }
        }

        // ---- drain control commands (READY only) ---------------
        if (s_cs.link == CAM_LINK_READY) {
            cam_cmd_t c;
            while (xQueueReceive(s_cmdq, &c, 0) == pdTRUE) { exec_cmd(&c); }
        }

        twdt_feed(WDT_CAM);
    }
}

void cam_start(void)
{
    cam_state_init();
    s_found_mux = xSemaphoreCreateMutexStatic(&s_found_mux_buf);
    s_iq   = xQueueCreateStatic(IQ_DEPTH, sizeof(iq_item_t), s_iq_buf, &s_iq_s);
    s_nb   = xMessageBufferCreateStatic(NB_SIZE, s_nb_buf, &s_nb_s);
    s_cmdq = xQueueCreateStatic(16, sizeof(cam_cmd_t), s_cmdq_buf, &s_cmdq_s);
    s_evq  = xQueueCreateStatic(16, sizeof(cam_event_t), s_evq_buf, &s_evq_s);
    cam_registry_set_cbs(&k_cbs);

    static StaticTask_t tcb;
    static StackType_t  stack[5120 / sizeof(StackType_t)];
    xTaskCreateStatic(cam_task, "cam_task", sizeof(stack)/sizeof(stack[0]),
                      NULL, 10, stack, &tcb);
}

// Liveness hook for housekeeping (spec 3.5): if READY but silent 30 s, force
// a teardown + reconnect (not a reboot).
void cam_liveness_check(void)
{
    cam_link_state_t link; uint32_t last_rx;
    cam_state_get_link(&link, &last_rx);     // not a full cam_state_t: housekeeping's stack is small
    if (link == CAM_LINK_READY && (now_ms() - last_rx) > LIVENESS_STALL_MS) {
        iq_item_t it = { .kind = IQ_BLE_EVT, .ble = { .ev = BLE_EVT_DISCONNECTED, .status = 0 } };
        xQueueSend(s_iq, &it, 0);
    }
}
