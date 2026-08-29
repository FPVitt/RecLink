// ble_central.c -- NimBLE central role glue. Deliberately minimal: one peer,
// one bond, notifications fanned out to a single callback. All bond keys persist
// in the "nimble_bond" NVS namespace (CONFIG_BT_NIMBLE_NVS_PERSIST=y, spec 11.0).
#include "ble_central.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble";

static ble_scan_cb_t   s_scan_cb;
static ble_evt_cb_t    s_evt_cb;
static ble_notify_cb_t s_notify_cb;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static int8_t   s_rssi;
static bool     s_host_synced;

#define MAX_CHRS 24
static struct { ble_uuid_any_t uuid; uint16_t val_handle; } s_chrs[MAX_CHRS];
static int s_chr_n;

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
        emit(BLE_EVT_DISCONNECTED, event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        emit(event->enc_change.status == 0 ? BLE_EVT_ENC_DONE : BLE_EVT_ENC_FAIL,
             event->enc_change.status);
        break;
    case BLE_GAP_EVENT_MTU:
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
static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && chr && s_chr_n < MAX_CHRS) {
        ble_uuid_copy(&s_chrs[s_chr_n].uuid, &chr->uuid.u);
        s_chrs[s_chr_n].val_handle = chr->val_handle;
        s_chr_n++;
    } else if (err->status == BLE_HS_EDONE) {
        emit(BLE_EVT_DISC_DONE, 0);
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
esp_err_t ble_central_scan(bool enable)
{
    if (!s_host_synced) { return ESP_ERR_INVALID_STATE; }
    if (!enable) { return ble_gap_disc_cancel() == 0 ? ESP_OK : ESP_OK; }

    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_disc_params p = { .itvl = 0x0060, .window = 0x0030, .passive = 0, .filter_duplicates = 1 };
    int rc = ble_gap_disc(own, BLE_HS_FOREVER, &p, gap_event, NULL);
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
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

esp_err_t ble_central_write(uint16_t h, const void *data, uint16_t len, bool with_rsp)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !h) { return ESP_ERR_INVALID_STATE; }
    int rc = with_rsp ? ble_gattc_write_flat(s_conn, h, data, len, NULL, NULL)
                      : ble_gattc_write_no_rsp_flat(s_conn, h, data, len);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_central_subscribe(uint16_t val_handle, bool enable)
{
    // CCCD sits at val_handle + 1 for well-behaved peers; both GoPro and DJI
    // follow this. A full descriptor discovery would be more robust (VERIFY).
    uint16_t cccd = val_handle + 1;
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

esp_err_t ble_central_forget_bond(const uint8_t addr[6], uint8_t addr_type)
{
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    return ble_store_util_delete_peer(&a) == 0 ? ESP_OK : ESP_FAIL;
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
    ble_svc_gap_device_name_set("CamBridge");

    nimble_port_freertos_init(host_task);
}
