// ble_central.c -- NimBLE central role glue. Deliberately minimal: one peer,
// one bond, notifications fanned out to a single callback. All bond keys persist
// in the "nimble_bond" NVS namespace (CONFIG_BT_NIMBLE_NVS_PERSIST=y, spec 11.0).
#include "ble_central.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "nvs_flash.h"
#include "monoclock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

void ble_store_config_init(void);   // esp-nimble store/config; no public header

static const char *TAG = "ble";

static ble_scan_cb_t   s_scan_cb;
static ble_evt_cb_t    s_evt_cb;
static ble_notify_cb_t s_notify_cb;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static int8_t   s_rssi;
static uint16_t s_mtu;
static bool     s_host_synced;

// A GoPro exposes ~30 characteristics (GAP/DIS, Wi-Fi AP, control+query, camera
// management, internal services). Too small a table silently hides the later
// ones -- including the control/query characteristics we need.
#define MAX_CHRS 48
static struct { ble_uuid_any_t uuid; uint16_t def_handle; uint16_t val_handle; uint16_t cccd_handle; } s_chrs[MAX_CHRS];
static int s_chr_n;
static int s_dsc_idx;       // characteristic whose descriptors are being discovered

// ---------------------------------------------------------------------------
// Serialized ATT write queue.
//
// ATT allows exactly ONE outstanding request per link. Firing write-REQUESTS
// back-to-back gets all but the first rejected by the host, and since callers
// are fire-and-forget (spec 1.2.4) nobody notices: that is why GoPro push
// registration never took effect -- gp_pull_all() issued 16 write-requests in
// a row and only the first reached the camera.
//
// So every write is queued here and sent one at a time: a write-REQUEST holds
// the queue until its completion callback lands; a write-COMMAND (no response)
// is unacknowledged and so is sent straight through. Enqueue happens on
// cam_task, completion on the NimBLE host task, hence the mutex -- held only
// to touch the ring, never across the GATT call itself.
// ---------------------------------------------------------------------------
#define WRQ_DEPTH     16
#define WRQ_DATA_MAX  72     // GoPro packets are <= 20 B; DUML frames <= ~64 B
#define WRQ_STALL_MS  3000   // a request outstanding this long = something is wrong

typedef struct {
    uint16_t handle;
    uint16_t len;
    bool     with_rsp;
    uint8_t  data[WRQ_DATA_MAX];
} wr_item_t;

static wr_item_t s_wrq[WRQ_DEPTH];
static uint8_t   s_wrq_head, s_wrq_tail, s_wrq_count;
static bool      s_wr_inflight;
static uint32_t  s_wr_inflight_ms;
static SemaphoreHandle_t s_wrq_lock;
static StaticSemaphore_t s_wrq_lock_buf;

static void wrq_lock(void)   { if (s_wrq_lock) { xSemaphoreTake(s_wrq_lock, portMAX_DELAY); } }
static void wrq_unlock(void) { if (s_wrq_lock) { xSemaphoreGive(s_wrq_lock); } }

static void wrq_reset(void)
{
    wrq_lock();
    s_wrq_head = s_wrq_tail = s_wrq_count = 0;
    s_wr_inflight = false;
    wrq_unlock();
}

static void wrq_pump(void);

static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) {
        ESP_LOGW(TAG, "ATT write failed: %d", err->status);
    }
    wrq_lock();
    s_wr_inflight = false;
    wrq_unlock();
    wrq_pump();          // next queued write, if any
    return 0;
}

// Sends queued writes until a write-request is left in flight (or the queue
// drains). Safe to call from either task; the GATT calls happen outside the lock.
static void wrq_pump(void)
{
    for (;;) {
        wr_item_t it;

        wrq_lock();
        if (s_wr_inflight) {
            // Self-heal: never let one lost completion wedge the link (and with
            // it the 3 s keep-alive, which would let the camera sleep).
            if ((uint32_t)(now_ms() - s_wr_inflight_ms) < WRQ_STALL_MS) { wrq_unlock(); return; }
            ESP_LOGW(TAG, "ATT write stalled %lu ms -- continuing",
                     (unsigned long)(now_ms() - s_wr_inflight_ms));
            s_wr_inflight = false;
        }
        if (s_wrq_count == 0 || s_conn == BLE_HS_CONN_HANDLE_NONE) { wrq_unlock(); return; }
        it = s_wrq[s_wrq_tail];
        s_wrq_tail = (uint8_t)((s_wrq_tail + 1) % WRQ_DEPTH);
        s_wrq_count--;
        if (it.with_rsp) { s_wr_inflight = true; s_wr_inflight_ms = now_ms(); }
        wrq_unlock();

        int rc;
        if (it.with_rsp) {
            rc = ble_gattc_write_flat(s_conn, it.handle, it.data, it.len, on_write_done, NULL);
            if (rc != 0) {
                // Nothing will complete it -- drop it and keep the queue moving.
                ESP_LOGW(TAG, "write_flat h=%u rc=%d", it.handle, rc);
                wrq_lock();
                s_wr_inflight = false;
                wrq_unlock();
                continue;
            }
            return;      // wait for on_write_done
        }
        rc = ble_gattc_write_no_rsp_flat(s_conn, it.handle, it.data, it.len);
        if (rc != 0) { ESP_LOGW(TAG, "write_no_rsp h=%u rc=%d", it.handle, rc); }
    }
}

// ---- helpers ---------------------------------------------------------
static void emit(ble_central_evt_t ev, int status)
{
    if (s_evt_cb) { s_evt_cb(ev, status); }
}

// ---- GAP event handler --------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg);

static void parse_adv(const struct ble_gap_disc_desc *d, ble_adv_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->addr, d->addr.val, 6);
    out->addr_type = d->addr.type;
    out->rssi = d->rssi;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) { return; }
    if (f.name && f.name_len) {
        uint8_t n = f.name_len < sizeof(out->name) - 1 ? f.name_len : sizeof(out->name) - 1;
        memcpy(out->name, f.name, n);
        out->name[n] = '\0';
    }
    if (f.mfg_data && f.mfg_data_len) {
        uint8_t n = f.mfg_data_len < sizeof(out->mfg) ? f.mfg_data_len : sizeof(out->mfg);
        memcpy(out->mfg, f.mfg_data, n);
        out->mfg_len = n;
    }
    for (int i = 0; i < f.num_uuids16; i++) {
        out->svc_uuid16 = ble_uuid_u16(&f.uuids16[i].u);
        break;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!s_scan_cb) { break; }
        ble_adv_t a;
        parse_adv(&event->disc, &a);
        s_scan_cb(&a);
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            emit(BLE_EVT_CONNECTED, 0);
            ble_gattc_exchange_mtu(s_conn, NULL, NULL);
        } else {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            emit(BLE_EVT_DISCONNECTED, event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_chr_n = 0;
        s_mtu = 0;
        wrq_reset();        // nothing queued is deliverable now (spec 5.1: drop, don't buffer)
        emit(BLE_EVT_DISCONNECTED, event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        emit(event->enc_change.status == 0 ? BLE_EVT_ENC_DONE : BLE_EVT_ENC_FAIL,
             event->enc_change.status);
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        emit(BLE_EVT_MTU_DONE, event->mtu.value);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (s_notify_cb && event->notify_rx.om) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[512];
            if (len > sizeof(buf)) { len = sizeof(buf); }
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            s_notify_cb(event->notify_rx.attr_handle, buf, len);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Delete the old bond and allow re-pair (spec: re-pair path).
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        break;
    }
    return 0;
}

// ---- GATT discovery ----------------------------------------------
// Two passes: characteristics, then descriptors one characteristic at a time.
// The second pass is what gives us real CCCD handles -- assuming val_handle+1
// happens to be right on today's GoPro and DJI firmware, but it is not
// guaranteed by GATT and would fail silently (subscribed to nothing, no
// notifications, no error) if either vendor ever inserted another descriptor.
// The camera does not persist CCCDs, so this runs on every connection.
//
// Descriptors must be discovered per characteristic: NimBLE reports the
// start_handle you passed as the "owning characteristic" in every callback, so
// one sweep over 1..0xffff attributes everything to handle 1 and matches nothing.
static void disc_next_dsc(uint16_t conn);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (err->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            s_chrs[s_dsc_idx].cccd_handle = dsc->handle;
        }
    } else {
        // EDONE, or an error for this one characteristic: keep going. A missing
        // CCCD falls back to val_handle+1 (with a warning) in ble_central_subscribe.
        if (err->status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "descriptor discovery h=%u failed: %d",
                     s_chrs[s_dsc_idx].val_handle, err->status);
        }
        s_dsc_idx++;
        disc_next_dsc(conn);
    }
    return 0;
}

// Descriptors of characteristic i live between its value handle and the next
// characteristic's declaration (the table is in handle order).
static void disc_next_dsc(uint16_t conn)
{
    for (; s_dsc_idx < s_chr_n; s_dsc_idx++) {
        uint16_t start = s_chrs[s_dsc_idx].val_handle + 1;
        uint16_t end = (s_dsc_idx + 1 < s_chr_n) ? (uint16_t)(s_chrs[s_dsc_idx + 1].def_handle - 1)
                                                 : 0xffff;
        if (start > end) { continue; }
        if (ble_gattc_disc_all_dscs(conn, start, end, on_dsc, NULL) == 0) { return; }
        ESP_LOGW(TAG, "descriptor discovery h=%u not started",
                 s_chrs[s_dsc_idx].val_handle);
    }
    emit(BLE_EVT_DISC_DONE, 0);     // characteristics + descriptors both done
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status == 0 && chr) {
        if (s_chr_n < MAX_CHRS) {
            ble_uuid_copy(&s_chrs[s_chr_n].uuid, &chr->uuid.u);
            s_chrs[s_chr_n].def_handle = chr->def_handle;
            s_chrs[s_chr_n].val_handle = chr->val_handle;
            s_chrs[s_chr_n].cccd_handle = 0;
            s_chr_n++;
        } else {
            static bool warned;
            if (!warned) { warned = true; ESP_LOGW(TAG, "GATT table > %d characteristics -- ignoring the rest", MAX_CHRS); }
        }
    } else if (err->status == BLE_HS_EDONE) {
        s_dsc_idx = 0;
        disc_next_dsc(conn);        // DISC_DONE is emitted when that finishes
    } else if (err->status != 0) {
        emit(BLE_EVT_DISC_FAIL, err->status);
    }
    return 0;
}

esp_err_t ble_central_discover_all(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) { return ESP_ERR_INVALID_STATE; }
    s_chr_n = 0;
    int rc = ble_gattc_disc_all_chrs(s_conn, 1, 0xffff, on_chr, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

uint16_t ble_central_val_handle(const ble_uuid_t *u)
{
    for (int i = 0; i < s_chr_n; i++) {
        if (ble_uuid_cmp(&s_chrs[i].uuid.u, u) == 0) { return s_chrs[i].val_handle; }
    }
    return 0;
}

// ---- ops --------------------------------------------------------
// "gentle" = low-duty passive scan for a bonded reconnect while the Wi-Fi portal
// AP is up: ~11% radio, no scan-request TX, so it coexists with Wi-Fi
// association / HTTP instead of starving it. Reconnect matches on address only,
// so the scan response (which carries the GoPro name) is not needed. Pairing and
// in-flight reconnect use the aggressive active scan.
static bool s_scan_gentle;
static bool s_scan_gentle_running;
void ble_central_scan_gentle(bool on) { s_scan_gentle = on; }

esp_err_t ble_central_scan(bool enable)
{
    if (!s_host_synced) { return ESP_ERR_INVALID_STATE; }
    if (!enable) { ble_gap_disc_cancel(); return ESP_OK; }
    if (ble_gap_disc_active()) {
        if (s_scan_gentle_running == s_scan_gentle) { return ESP_OK; }
        ble_gap_disc_cancel();          // mode changed -> restart with new params
    }

    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own);
    // No duplicate filtering -- some controllers suppress scan responses with it.
    struct ble_gap_disc_params p = s_scan_gentle
        ? (struct ble_gap_disc_params){ .itvl = 0x0140, .window = 0x0024, .passive = 1, .filter_duplicates = 0 }
        : (struct ble_gap_disc_params){ .itvl = 0x0060, .window = 0x0050, .passive = 0, .filter_duplicates = 0 };
    int rc = ble_gap_disc(own, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) { s_scan_gentle_running = s_scan_gentle; return ESP_OK; }
    return ESP_FAIL;
}

esp_err_t ble_central_connect(const uint8_t addr[6], uint8_t addr_type)
{
    ble_gap_disc_cancel();
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x0010, .scan_window = 0x0010,
        .itvl_min = 24, .itvl_max = 40, .latency = 0,
        .supervision_timeout = 400,
        .min_ce_len = 0, .max_ce_len = 0,
    };
    int rc = ble_gap_connect(own, &a, 8000, &cp, gap_event, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_central_secure(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) { return ESP_ERR_INVALID_STATE; }
    int rc = ble_gap_security_initiate(s_conn);
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}

// Queues the write; ESP_OK means "accepted for transmission", not "delivered"
// (the fire-and-forget contract, spec 1.2.4). See the write-queue comment above.
esp_err_t ble_central_write(uint16_t h, const void *data, uint16_t len, bool with_rsp)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !h) { return ESP_ERR_INVALID_STATE; }
    if (len > WRQ_DATA_MAX) {
        ESP_LOGE(TAG, "write of %u B exceeds queue slot (%u)", len, (unsigned)WRQ_DATA_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    wrq_lock();
    if (s_wrq_count >= WRQ_DEPTH) {
        wrq_unlock();
        ESP_LOGE(TAG, "ATT write queue full -- dropped write to h=%u", h);
        return ESP_ERR_NO_MEM;
    }
    wr_item_t *it = &s_wrq[s_wrq_head];
    it->handle = h;
    it->len = len;
    it->with_rsp = with_rsp;
    memcpy(it->data, data, len);
    s_wrq_head = (uint8_t)((s_wrq_head + 1) % WRQ_DEPTH);
    s_wrq_count++;
    wrq_unlock();

    wrq_pump();
    return ESP_OK;
}

esp_err_t ble_central_subscribe(uint16_t val_handle, bool enable)
{
    // Real CCCD handle from descriptor discovery. The val_handle+1 fallback
    // only covers a peer whose descriptors we failed to enumerate -- it is what
    // both vendors happen to use, but it is an assumption, so say so when we
    // have to lean on it.
    uint16_t cccd = 0;
    for (int i = 0; i < s_chr_n; i++) {
        if (s_chrs[i].val_handle == val_handle) { cccd = s_chrs[i].cccd_handle; break; }
    }
    if (!cccd) {
        cccd = val_handle + 1;
        ESP_LOGW(TAG, "no CCCD discovered for val handle %u -- assuming %u", val_handle, cccd);
    }
    uint16_t v = enable ? 1 : 0;
    return ble_central_write(cccd, &v, sizeof(v), true);
}

void ble_central_disconnect(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool   ble_central_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
int8_t ble_central_last_rssi(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) { ble_gap_conn_rssi(s_conn, &s_rssi); }
    return s_rssi;
}
uint16_t ble_central_mtu(void) { return s_mtu; }

esp_err_t ble_central_forget_bond(const uint8_t addr[6], uint8_t addr_type)
{
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    return ble_store_util_delete_peer(&a) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_central_forget_bond_any(const uint8_t addr[6])
{
    ble_addr_t a;
    memcpy(a.val, addr, 6);
    bool ok = false;
    for (uint8_t t = 0; t <= 1; t++) {
        a.type = t;
        if (ble_store_util_delete_peer(&a) == 0) { ok = true; }
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_central_erase_all_bonds(void)
{
    int rc = ble_store_clear();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

int ble_central_bond_count(void)
{
    int count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    return count;
}

// ---- host lifecycle --------------------------------------------
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_host_synced = true;
    ESP_LOGI(TAG, "host synced");
}
static void on_reset(int reason) { ESP_LOGW(TAG, "host reset %d", reason); s_host_synced = false; }

bool ble_central_synced(void) { return s_host_synced; }

static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_central_set_cbs(ble_scan_cb_t scan, ble_evt_cb_t evt, ble_notify_cb_t notify)
{
    s_scan_cb = scan; s_evt_cb = evt; s_notify_cb = notify;
}

void ble_central_init(void)
{
    static bool done;
    if (done) { return; }
    done = true;

    // Guards the ATT write ring, which cam_task fills and the host task drains.
    s_wrq_lock = xSemaphoreCreateMutexStatic(&s_wrq_lock_buf);

    (void)nimble_port_init();

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Just-Works bonding, no MITM, persistent (spec 6.1 / 7.3).
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("RecLink");

    // Wire up the NVS-backed bond-key store (CONFIG_BT_NIMBLE_NVS_PERSIST). The
    // IDF NimBLE port does NOT call this for you -- without it sm_bonding has
    // nowhere to persist keys and pairing fails (matches the blecent example).
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}

// RF_PWR_LOW/MEDIUM/MAX (cfg.h) -> esp_power_level_t. Sets the default for
// every BLE role (adv/scan/connections) that hasn't been given its own level.
// Call after ble_central_init(); takes effect immediately, but since NimBLE
// itself is only ever brought up once at boot, changing cfg.ble_tx_power needs
// a reboot to actually re-apply.
void ble_central_set_tx_power(uint8_t preset)
{
    static const esp_power_level_t k_levels[] = {
        ESP_PWR_LVL_N12,   // RF_PWR_LOW    -12 dBm
        ESP_PWR_LVL_N0,    // RF_PWR_MEDIUM   0 dBm
        ESP_PWR_LVL_P9,    // RF_PWR_MAX     +9 dBm (matches the NimBLE/BT default)
    };
    if (preset > 2) { preset = 2; }
    esp_err_t e = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, k_levels[preset]);
    if (e != ESP_OK) { ESP_LOGW(TAG, "ble tx power: %s", esp_err_to_name(e)); }
}
