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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "cam";

// ---- timings (spec 5.2 / 8.5) ----------------------------------------
#define CAM_RX_TIMEOUT_MS     5000
#define CONNECT_TIMEOUT_MS   10000
#define TICK_PERIOD_MS        1000
#define LIVENESS_STALL_MS    30000     // housekeeping-style check, spec 3.5

static const uint32_t k_backoff_s[] = { 1, 2, 4, 8, 16, 30, 30 };

// ---- internal event queue -----------------------------------------
typedef enum { IQ_BLE_EVT, IQ_NOTIFY, IQ_ADV, IQ_CMD_PAIR, IQ_CMD_FORGET } iq_kind_t;
typedef struct {
    uint8_t kind;
    union {
        struct { int ev; int status; } ble;
        struct { uint16_t handle; uint16_t len; uint8_t data[256]; } notify;
        ble_adv_t adv;                    // full advert; cam_task does the matching
        struct { char brand[6]; } pair;
    };
} iq_item_t;

static StaticQueue_t s_iq_s, s_cmdq_s, s_evq_s;
static uint8_t s_iq_buf[8 * sizeof(iq_item_t)];
static uint8_t s_cmdq_buf[16 * sizeof(cam_cmd_t)];
static uint8_t s_evq_buf[16 * sizeof(cam_event_t)];
static QueueHandle_t s_iq, s_cmdq, s_evq;

// ---- driver + link bookkeeping --------------------------------------
static const camera_driver_t *s_drv;
static bool     s_first_pair;
static uint8_t  s_target_addr[6];
static uint8_t  s_target_addr_type;
static int      s_backoff_idx;
static uint32_t s_state_since_ms;
// The one shared camera-state scratch (spec 3.4). cam_task + the driver
// callbacks it invokes all mutate *cam_scratch()*; cam_state_commit() publishes.
#define s_cs (*cam_scratch())

// ---- helpers ----------------------------------------------------
static void set_link(cam_link_state_t st)
{
    if (s_cs.link == st) { return; }
    s_cs.link = st;
    s_state_since_ms = now_ms();
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
    s_cs.busy = TRIBOOL_UNKNOWN;
    s_cs.battery_pct.valid = false;
    s_cs.sd_remaining_kb.valid = false;
    s_cs.rec_elapsed_s.valid = false;
    s_cs.rec_remaining_s.valid = false;
    s_cs.resolution.valid = false;
    s_cs.framerate.valid = false;
    s_cs.lens.valid = false;
    s_cs.mode.valid = false;
    s_cs.mode_name.valid = false;
    s_cs.cam_utc_ms.valid = false;
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
    cfg_set_camera_bond(s_drv->name, addr, name, dji_id);
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
static void h_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    iq_item_t it = { .kind = IQ_NOTIFY };
    it.notify.handle = handle;
    it.notify.len = len > sizeof(it.notify.data) ? sizeof(it.notify.data) : len;
    memcpy(it.notify.data, data, it.notify.len);
    xQueueSend(s_iq, &it, 0);
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
esp_err_t cam_forget(void)
{
    iq_item_t it = { .kind = IQ_CMD_FORGET };
    return xQueueSend(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
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
    const cfg_t *c = cfg_get();
    int bonds = ble_central_bond_count();
    if (c->camera.bonded && bonds == 0) {
        applog_note("cfg says bonded but no BLE bond -> REPAIR_NEEDED");
        return true;                          // caller sets flag; cam stays IDLE-ish
    }
    if (!c->camera.bonded && bonds > 0) {
        applog_note("orphan BLE bond, no cfg camera -> erasing");
        ble_central_erase_all_bonds();
    }
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
    }
}

// ---- state machine step --------------------------------------
static void begin_scan(void)
{
    invalidate_cam_values();
    set_link(CAM_LINK_SCAN);
    ble_central_scan(true);
}

static void go_lost(const char *why)
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
        set_link(CAM_LINK_SYNCING);
        ble_central_discover_all();
        break;
    case BLE_EVT_ENC_FAIL:
        go_lost("bond failed");
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
        if (r != ESP_OK) { go_lost("driver link_up failed"); break; }
        // Driver must call CB->on_link_state(CAM_LINK_READY) once its protocol
        // sync completes (GoPro get-all returns / DUML handshake acks). The
        // CONNECT_TIMEOUT_MS guard covers a driver that never gets there.
        s_backoff_idx = 0;
        s_cs.last_rx_ms = now_ms();
        if (s_first_pair) {
            // driver's on_paired persisted cfg; clear the first-pair flag
            s_first_pair = false;
        }
        break;
    }
    case BLE_EVT_DISC_FAIL:
        go_lost("gatt discovery failed");
        break;
    case BLE_EVT_DISCONNECTED:
        if (s_cs.link != CAM_LINK_IDLE) { go_lost("gatt disconnect"); }
        break;
    }
}

static void cam_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_CAM);
    ble_central_set_cbs(h_scan, h_evt, h_notify);

    const cfg_t *cfg = cfg_get();
    if (bridge_flag_get(FLAG_REPAIR_NEEDED)) {
        ESP_LOGW(TAG, "REPAIR_NEEDED: staying IDLE until re-pair");
        set_link(CAM_LINK_IDLE);
    } else if (cfg && cfg->camera.bonded && cfg->camera.brand[0]) {
        s_drv = camera_by_name(cfg->camera.brand);
        if (s_drv) {
            memcpy(s_target_addr, cfg->camera.addr, 6);
            s_target_addr_type = 0;   // public; VERIFY per camera (GoPro random?)
            s_first_pair = false;
            begin_scan();
        }
    } else {
        set_link(CAM_LINK_IDLE);      // pairing only via portal (spec 3.6 step 5)
    }

    uint32_t last_tick = now_ms();

    for (;;) {
        iq_item_t it;
        if (xQueueReceive(s_iq, &it, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (it.kind) {
            case IQ_ADV: {
                if (s_cs.link != CAM_LINK_SCAN || !s_drv) { break; }
                bool want = s_first_pair
                    ? (s_drv->adv_match && s_drv->adv_match(&it.adv))
                    : (memcmp(it.adv.addr, s_target_addr, 6) == 0);
                if (!want) { break; }
                memcpy(s_target_addr, it.adv.addr, 6);
                s_target_addr_type = it.adv.addr_type;
                memcpy(s_cs.addr, it.adv.addr, 6);
                if (it.adv.name[0]) { strlcpy(s_cs.name, it.adv.name, sizeof(s_cs.name)); }
                ble_central_scan(false);
                set_link(CAM_LINK_CONNECTING);
                ble_central_connect(s_target_addr, s_target_addr_type);
                break;
            }
            case IQ_BLE_EVT:
                handle_ble_evt(it.ble.ev, it.ble.status);
                break;
            case IQ_NOTIFY:
                s_cs.last_rx_ms = now_ms();
                if (s_drv && s_drv->on_notify) {
                    s_drv->on_notify(it.notify.handle, it.notify.data, it.notify.len);
                }
                break;
            case IQ_CMD_PAIR: {
                const camera_driver_t *d = camera_by_name(it.pair.brand);
                if (d) {
                    ESP_LOGI(TAG, "pairing: %s", it.pair.brand);
                    if (ble_central_connected()) { ble_central_disconnect(); }
                    s_drv = d;
                    s_first_pair = true;
                    s_backoff_idx = 0;
                    begin_scan();
                }
                break;
            }
            case IQ_CMD_FORGET:
                ble_central_disconnect();
                if (cfg_get()->camera.bonded) {
                    ble_central_forget_bond(cfg_get()->camera.addr, s_target_addr_type);
                }
                cfg_clear_camera();
                s_drv = NULL;
                memset(s_target_addr, 0, 6);
                invalidate_cam_values();
                set_link(CAM_LINK_IDLE);
                applog_note("camera forgotten");
                break;
            }
        }

        uint32_t now = now_ms();

        // ---- periodic 1 Hz ---------------------------------------
        if (now - last_tick >= TICK_PERIOD_MS) {
            last_tick = now;
            if (s_cs.link == CAM_LINK_READY) {
                if (s_drv && s_drv->tick_1s) { s_drv->tick_1s(); }
                s_cs.rssi = ble_central_last_rssi();

                // RX-silence timeout (spec 8.5)
                if (now - s_cs.last_rx_ms > CAM_RX_TIMEOUT_MS) {
                    go_lost("rx timeout 5 s");
                }
            }

            // local elapsed-time counter for GoPro (spec 6.5)
            if (s_cs.recording == TRIBOOL_TRUE && s_cs.rec_elapsed_s.valid) {
                // driver increments if it owns it; nothing here.
            }
            publish_dirty();
        }

        // ---- connect / sync timeout -----------------------------
        if ((s_cs.link == CAM_LINK_CONNECTING || s_cs.link == CAM_LINK_SYNCING) &&
            now - s_state_since_ms > CONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "connect/sync timeout -> rescan");
            ble_central_disconnect();
            begin_scan();
        }

        // ---- LOST backoff -> rescan ----------------------------
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
    s_iq   = xQueueCreateStatic(8, sizeof(iq_item_t), s_iq_buf, &s_iq_s);
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
    cam_state_t s;
    cam_state_get(&s);
    if (s.link == CAM_LINK_READY && (now_ms() - s.last_rx_ms) > LIVENESS_STALL_MS) {
        iq_item_t it = { .kind = IQ_BLE_EVT, .ble = { .ev = BLE_EVT_DISCONNECTED, .status = 0 } };
        xQueueSend(s_iq, &it, 0);
    }
}
