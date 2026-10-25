// dbg_ble.c -- see dbg_ble.h.
//
// Threading: NimBLE GAP/GATT callbacks run on the NimBLE host task. Anything
// that issues an ATT request (discovery, write, read, CCCD, MTU) is posted to
// gatt_task, which runs exactly ONE procedure at a time and waits for its
// completion callback -- ATT allows a single outstanding request per link, and
// firing writes back-to-back silently drops some of them.
#include "dbg_ble.h"
#include "evlog.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

static const char *TAG = "dbg_ble";

#define OP_TIMEOUT_MS   6000
#define GP_PKT_MAX      20          // Open GoPro: 20-byte packets (BLE 4.2 sized)
#define GP_MSG_MAX      8191        // 13-bit extended header limit
#define DEV_MAX         20

// GoPro 128-bit base b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b (NimBLE = little-endian)
static const uint8_t k_gp_base[16] = { 0x1b,0xc5,0xd5,0xa5,0x02,0x00,0x46,0x90,
                                       0xe3,0x11,0x8d,0xaa,0x00,0x00,0xf9,0xb5 };

// ---- shared state (guarded by s_mux) ---------------------------------
static SemaphoreHandle_t s_mux;
static dbg_status_t s_st = { .conn = BLE_HS_CONN_HANDLE_NONE, .keepalive_ms = 3000,
                             .auto_secure = true, .auto_discover = true, .auto_subscribe = true };
static dbg_gatt_t   s_gatt;
static dbg_dev_t    s_devs[DEV_MAX];
static int          s_dev_n;
static uint16_t     s_h_settings;   // GP-0074 value handle, for keep-alive

#define LOCK()   xSemaphoreTake(s_mux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mux)

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- GATT worker -------------------------------------------------------
typedef enum { OP_WRITE, OP_READ, OP_DISCOVER, OP_SUB, OP_SUB_ALL, OP_MTU } op_kind_t;
typedef struct {
    uint8_t  kind;
    uint8_t  gopro, rsp, mode, flags;
    uint16_t handle;
    uint16_t len;
    uint8_t *buf;                     // heap; worker frees
} op_t;

static QueueHandle_t     s_opq;
static SemaphoreHandle_t s_done;
static volatile uint32_t s_op_id;     // completion callbacks carry the id they belong to
static volatile int      s_op_status;
// discovery scratch, filled by callbacks of the in-flight procedure only
static dbg_gatt_t        s_disc;
static int               s_disc_chr_idx;
// read scratch
static uint8_t           s_rd[512];
static uint16_t          s_rd_len;

static void op_finish(uint32_t id, int status)
{
    if (id != s_op_id) { return; }    // stale callback from a timed-out op
    s_op_status = status;
    xSemaphoreGive(s_done);
}

static int op_wait(void)
{
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(OP_TIMEOUT_MS)) != pdTRUE) {
        s_op_id++;                    // orphan whatever is still in flight
        return BLE_HS_ETIMEOUT;
    }
    return s_op_status;
}

static uint16_t conn_handle(void)
{
    LOCK(); uint16_t c = s_st.conn; UNLOCK();
    return c;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr;
    op_finish((uint32_t)(uintptr_t)arg, err->status);
    return 0;
}

// One ATT write; with_rsp waits for the Write Response.
static int att_write(uint16_t h, const uint8_t *p, uint16_t n, bool with_rsp, uint8_t flags)
{
    uint16_t c = conn_handle();
    if (c == BLE_HS_CONN_HANDLE_NONE) { return BLE_HS_ENOTCONN; }
    int rc;
    if (with_rsp) {
        uint32_t id = ++s_op_id;
        xSemaphoreTake(s_done, 0);
        rc = ble_gattc_write_flat(c, h, p, n, on_write, (void *)(uintptr_t)id);
        if (rc == 0) { rc = op_wait(); }
    } else {
        rc = ble_gattc_write_no_rsp_flat(c, h, p, n);
        for (int tries = 0; rc == BLE_HS_ENOMEM && tries < 50; tries++) {   // mbuf pool drained
            vTaskDelay(pdMS_TO_TICKS(10));
            rc = ble_gattc_write_no_rsp_flat(c, h, p, n);
        }
    }
    evlog_push(EV_TX, h, (int16_t)rc, flags, p, n);
    LOCK();
    s_st.tx_pkts++;
    if (rc) { s_st.tx_errs++; }
    UNLOCK();
    return rc;
}

// Open GoPro packetizer (spec "Data Protocol"): <= 19 B payload -> one packet
// with the 5-bit general header; longer -> 13-bit extended start header then
// continuation packets 0x80|counter (counter wraps after 0xF).
static int gp_send(uint16_t h, const uint8_t *msg, size_t len, bool with_rsp, uint8_t flags)
{
    uint8_t pkt[GP_PKT_MAX];
    if (len < GP_PKT_MAX) {
        pkt[0] = (uint8_t)len;
        memcpy(pkt + 1, msg, len);
        return att_write(h, pkt, (uint16_t)(len + 1), with_rsp, flags);
    }
    size_t off = 0;
    uint8_t cnt = 0;
    while (off < len) {
        size_t hdr, room;
        if (off == 0) {
            pkt[0] = 0x20 | (uint8_t)((len >> 8) & 0x1F);
            pkt[1] = (uint8_t)len;
            hdr = 2;
        } else {
            pkt[0] = 0x80 | (cnt++ & 0x0F);
            hdr = 1;
        }
        room = GP_PKT_MAX - hdr;
        size_t n = len - off < room ? len - off : room;
        memcpy(pkt + hdr, msg + off, n);
        int rc = att_write(h, pkt, (uint16_t)(hdr + n), with_rsp, flags);
        if (rc) { return rc; }
        off += n;
    }
    return 0;
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    uint32_t id = (uint32_t)(uintptr_t)arg;
    if (err->status == 0 && attr && attr->om) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (s_rd_len + n > sizeof(s_rd)) { n = sizeof(s_rd) - s_rd_len; }
        os_mbuf_copydata(attr->om, 0, n, s_rd + s_rd_len);
        s_rd_len += n;
        return 0;
    }
    op_finish(id, err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static void do_read(uint16_t h)
{
    uint16_t c = conn_handle();
    int rc = BLE_HS_ENOTCONN;
    s_rd_len = 0;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        uint32_t id = ++s_op_id;
        xSemaphoreTake(s_done, 0);
        rc = ble_gattc_read_long(c, h, 0, on_read, (void *)(uintptr_t)id);
        if (rc == 0) { rc = op_wait(); }
    }
    evlog_push(EV_READ, h, (int16_t)rc, s_rd_len > EV_DATA_MAX ? EVF_TRUNC : 0, s_rd, s_rd_len);
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    (void)conn;
    if (err->status == 0) { LOCK(); s_st.mtu = mtu; UNLOCK(); }
    op_finish((uint32_t)(uintptr_t)arg, err->status);
    return 0;
}

static void do_mtu(void)
{
    uint16_t c = conn_handle();
    if (c == BLE_HS_CONN_HANDLE_NONE) { return; }
    uint32_t id = ++s_op_id;
    xSemaphoreTake(s_done, 0);
    int rc = ble_gattc_exchange_mtu(c, on_mtu, (void *)(uintptr_t)id);
    if (rc == 0) { rc = op_wait(); }
    LOCK(); uint16_t m = s_st.mtu; UNLOCK();
    if (rc) { evlog_text(EV_ERR, "MTU exchange failed (%d)", rc); }
    else    { evlog_text(EV_SYS, "MTU = %u", m); }
}

// ---- discovery: services -> characteristics -> descriptors -----------
static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn;
    if (err->status == 0 && svc) {
        if (s_disc.n_svc < DBG_MAX_SVC) {
            dbg_svc_t *s = &s_disc.svcs[s_disc.n_svc++];
            ble_uuid_copy(&s->uuid, &svc->uuid.u);
            s->start = svc->start_handle;
            s->end = svc->end_handle;
        }
        return 0;
    }
    op_finish((uint32_t)(uintptr_t)arg, err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int s_disc_svc_idx;
static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    if (err->status == 0 && chr) {
        if (s_disc.n_chr < DBG_MAX_CHR) {
            dbg_chr_t *c = &s_disc.chrs[s_disc.n_chr++];
            memset(c, 0, sizeof(*c));
            ble_uuid_copy(&c->uuid, &chr->uuid.u);
            c->def_handle = chr->def_handle;
            c->val_handle = chr->val_handle;
            c->props = chr->properties;
            c->svc = (uint8_t)s_disc_svc_idx;
        }
        return 0;
    }
    op_finish((uint32_t)(uintptr_t)arg, err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)chr_val_handle;
    if (err->status == 0 && dsc) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_disc.chrs[s_disc_chr_idx].cccd = dsc->handle;
        }
        return 0;
    }
    op_finish((uint32_t)(uintptr_t)arg, err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static bool is_gp_uuid(const ble_uuid_any_t *u, uint8_t lo)
{
    if (u->u.type != BLE_UUID_TYPE_128) { return false; }
    uint8_t want[16];
    memcpy(want, k_gp_base, 16);
    want[12] = lo;
    return memcmp(u->u128.value, want, 16) == 0;
}

static void do_discover(void)
{
    uint16_t c = conn_handle();
    if (c == BLE_HS_CONN_HANDLE_NONE) { evlog_text(EV_ERR, "discover: not connected"); return; }
    memset(&s_disc, 0, sizeof(s_disc));
    uint32_t t0 = now_ms();

    uint32_t id = ++s_op_id;
    xSemaphoreTake(s_done, 0);
    int rc = ble_gattc_disc_all_svcs(c, on_svc, (void *)(uintptr_t)id);
    if (rc == 0) { rc = op_wait(); }
    if (rc) { evlog_text(EV_ERR, "service discovery failed (%d)", rc); return; }

    for (int i = 0; i < s_disc.n_svc; i++) {
        s_disc_svc_idx = i;
        id = ++s_op_id;
        xSemaphoreTake(s_done, 0);
        rc = ble_gattc_disc_all_chrs(c, s_disc.svcs[i].start, s_disc.svcs[i].end,
                                     on_chr, (void *)(uintptr_t)id);
        if (rc == 0) { rc = op_wait(); }
        if (rc && rc != BLE_HS_EDONE) { evlog_text(EV_ERR, "chr discovery svc %d failed (%d)", i, rc); }
    }
    // Each characteristic's descriptors live between its value handle and the
    // next declaration (or the end of its service).
    for (int i = 0; i < s_disc.n_chr; i++) {
        dbg_chr_t *ch = &s_disc.chrs[i];
        uint16_t end = s_disc.svcs[ch->svc].end;
        if (i + 1 < s_disc.n_chr && s_disc.chrs[i + 1].svc == ch->svc) {
            end = s_disc.chrs[i + 1].def_handle - 1;
        }
        ch->end_handle = end;
        if (end <= ch->val_handle) { continue; }
        s_disc_chr_idx = i;
        id = ++s_op_id;
        xSemaphoreTake(s_done, 0);
        rc = ble_gattc_disc_all_dscs(c, ch->val_handle, end, on_dsc, (void *)(uintptr_t)id);
        if (rc == 0) { rc = op_wait(); }
        if (rc && rc != BLE_HS_EDONE) { evlog_text(EV_ERR, "dsc discovery h%u failed (%d)", ch->val_handle, rc); }
    }
    s_disc.complete = true;

    uint16_t hset = 0;
    for (int i = 0; i < s_disc.n_chr; i++) {
        if (is_gp_uuid(&s_disc.chrs[i].uuid, 0x74)) { hset = s_disc.chrs[i].val_handle; }
    }
    LOCK();
    s_gatt = s_disc;
    s_h_settings = hset;
    UNLOCK();
    evlog_text(EV_SYS, "GATT discovered: %d services, %d characteristics (%lu ms)",
               s_disc.n_svc, s_disc.n_chr, (unsigned long)(now_ms() - t0));
}

static void do_subscribe(uint16_t val_handle, uint8_t mode)
{
    uint16_t cccd = 0;
    int idx = -1;
    LOCK();
    for (int i = 0; i < s_gatt.n_chr; i++) {
        if (s_gatt.chrs[i].val_handle == val_handle) { idx = i; cccd = s_gatt.chrs[i].cccd; }
    }
    UNLOCK();
    if (!cccd) { cccd = val_handle + 1; }   // not discovered: conventional layout
    uint8_t v[2] = { mode == 1 ? 1 : 0, mode == 2 ? 1 : 0 };   // 0x0001 notify / 0x0002 indicate
    int rc = att_write(cccd, v, 2, true, 0);
    if (rc == 0 && idx >= 0) { LOCK(); s_gatt.chrs[idx].sub = mode; UNLOCK(); }
    evlog_text(rc ? EV_ERR : EV_SYS, "%s h%u (cccd h%u): %d",
               mode == 0 ? "unsubscribe" : mode == 1 ? "notify on" : "indicate on",
               val_handle, cccd, rc);
}

static void do_subscribe_all(void)
{
    // Spec: subscribe to every notifiable characteristic, on every connection
    // (the camera does not persist CCCDs).
    static uint16_t hs[DBG_MAX_CHR];
    static uint8_t  ms[DBG_MAX_CHR];
    int n = 0;
    LOCK();
    for (int i = 0; i < s_gatt.n_chr; i++) {
        const dbg_chr_t *c = &s_gatt.chrs[i];
        if (!c->cccd) { continue; }
        if (c->props & BLE_GATT_CHR_PROP_NOTIFY)        { hs[n] = c->val_handle; ms[n++] = 1; }
        else if (c->props & BLE_GATT_CHR_PROP_INDICATE) { hs[n] = c->val_handle; ms[n++] = 2; }
    }
    UNLOCK();
    if (!n) { evlog_text(EV_ERR, "subscribe all: no notifiable characteristics (discover first)"); }
    for (int i = 0; i < n; i++) { do_subscribe(hs[i], ms[i]); }
}

static void gatt_task(void *arg)
{
    (void)arg;
    uint32_t last_ka = 0;
    for (;;) {
        op_t op;
        if (xQueueReceive(s_opq, &op, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (op.kind) {
            case OP_WRITE:
                if (op.gopro) { gp_send(op.handle, op.buf, op.len, op.rsp, op.flags); }
                else          { att_write(op.handle, op.buf, op.len, op.rsp, op.flags); }
                break;
            case OP_READ:     do_read(op.handle); break;
            case OP_DISCOVER: do_discover(); break;
            case OP_SUB:      do_subscribe(op.handle, op.mode); break;
            case OP_SUB_ALL:  do_subscribe_all(); break;
            case OP_MTU:      do_mtu(); break;
            }
            free(op.buf);
        }
        // Keep-alive: Setting 91 (LED) = 0x42 on GP-0074, spec-recommended 3 s.
        LOCK();
        bool ka = s_st.keepalive && s_st.link == DBG_LINK_CONNECTED && s_h_settings;
        uint32_t per = s_st.keepalive_ms;
        uint16_t h = s_h_settings;
        UNLOCK();
        if (ka && now_ms() - last_ka >= per) {
            last_ka = now_ms();
            static const uint8_t k[] = { 0x5B, 0x01, 0x42 };
            gp_send(h, k, sizeof(k), true, EVF_KEEPALIVE);
        }
    }
}

static esp_err_t post(op_t *op, const void *data, size_t len)
{
    op->buf = NULL;
    op->len = (uint16_t)len;
    if (len) {
        op->buf = malloc(len);
        if (!op->buf) { return ESP_ERR_NO_MEM; }
        memcpy(op->buf, data, len);
    }
    if (xQueueSend(s_opq, op, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(op->buf);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t need_conn(void)
{
    return conn_handle() == BLE_HS_CONN_HANDLE_NONE ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t dbg_ble_discover(void)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    op_t op = { .kind = OP_DISCOVER };
    return post(&op, NULL, 0);
}

esp_err_t dbg_ble_subscribe(uint16_t h, uint8_t mode)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    op_t op = { .kind = OP_SUB, .handle = h, .mode = mode };
    return post(&op, NULL, 0);
}

esp_err_t dbg_ble_subscribe_all(void)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    op_t op = { .kind = OP_SUB_ALL };
    return post(&op, NULL, 0);
}

esp_err_t dbg_ble_write(uint16_t h, const uint8_t *data, size_t len, bool gopro, bool with_rsp)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    if (!h || len == 0 || len > (gopro ? GP_MSG_MAX : 512)) { return ESP_ERR_INVALID_ARG; }
    op_t op = { .kind = OP_WRITE, .handle = h, .gopro = gopro, .rsp = with_rsp };
    return post(&op, data, len);
}

esp_err_t dbg_ble_read(uint16_t h)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    op_t op = { .kind = OP_READ, .handle = h };
    return post(&op, NULL, 0);
}

esp_err_t dbg_ble_mtu(void)
{
    if (need_conn()) { return ESP_ERR_INVALID_STATE; }
    op_t op = { .kind = OP_MTU };
    return post(&op, NULL, 0);
}

void dbg_ble_keepalive(bool on, uint32_t period_ms)
{
    if (period_ms < 500) { period_ms = 500; }
    LOCK();
    s_st.keepalive = on;
    s_st.keepalive_ms = period_ms;
    UNLOCK();
}

// ---- GAP ------------------------------------------------------------------
static int gap_event(struct ble_gap_event *ev, void *arg);

static bool addr_bonded(const ble_addr_t *a)
{
    struct ble_store_key_sec k = { .peer_addr = *a, .idx = 0 };
    struct ble_store_value_sec v;
    return ble_store_read_peer_sec(&k, &v) == 0;
}

static void note_adv(const struct ble_gap_disc_desc *d)
{
    bool is_rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    struct ble_hs_adv_fields f;
    bool parsed = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0;

    LOCK();
    int i;
    for (i = 0; i < s_dev_n; i++) {
        if (memcmp(s_devs[i].addr, d->addr.val, 6) == 0 && s_devs[i].addr_type == d->addr.type) { break; }
    }
    if (i == s_dev_n) {
        if (s_dev_n == DEV_MAX) {               // evict the stalest entry
            i = 0;
            for (int j = 1; j < DEV_MAX; j++) { if (s_devs[j].last_ms < s_devs[i].last_ms) { i = j; } }
        } else {
            s_dev_n++;
        }
        memset(&s_devs[i], 0, sizeof(s_devs[i]));
        memcpy(s_devs[i].addr, d->addr.val, 6);
        s_devs[i].addr_type = d->addr.type;
    }
    dbg_dev_t *e = &s_devs[i];
    e->rssi = d->rssi;
    e->last_ms = now_ms();
    uint8_t n = d->length_data > 31 ? 31 : d->length_data;
    if (is_rsp) { memcpy(e->rsp, d->data, n); e->rsp_len = n; }
    else        { memcpy(e->adv, d->data, n); e->adv_len = n; }
    if (parsed) {
        if (f.name && f.name_len) {
            uint8_t m = f.name_len < sizeof(e->name) - 1 ? f.name_len : sizeof(e->name) - 1;
            memcpy(e->name, f.name, m);
            e->name[m] = '\0';
        }
        for (int k = 0; k < f.num_uuids16; k++) {
            if (ble_uuid_u16(&f.uuids16[k].u) == 0xFEA6) { e->gopro = 1; }
        }
        if (f.mfg_data_len >= 2 && f.mfg_data[0] == 0xF2 && f.mfg_data[1] == 0x02) { e->gopro = 1; }
        if (f.svc_data_uuid16_len >= 2 && f.svc_data_uuid16[0] == 0xA6 && f.svc_data_uuid16[1] == 0xFE) { e->gopro = 1; }
    }
    UNLOCK();
}

static void refresh_sec(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) { return; }
    LOCK();
    s_st.encrypted = desc.sec_state.encrypted;
    s_st.bonded = desc.sec_state.bonded;
    s_st.authenticated = desc.sec_state.authenticated;
    s_st.key_size = desc.sec_state.key_size;
    UNLOCK();
}

static void after_secure_or_connect(bool secured)
{
    LOCK();
    bool disc = s_st.auto_discover, sub = s_st.auto_subscribe, sec = s_st.auto_secure;
    UNLOCK();
    if (sec && !secured) { return; }        // wait for ENC_CHANGE
    if (disc) {
        op_t op = { .kind = OP_DISCOVER };
        post(&op, NULL, 0);
        if (sub) { op_t o2 = { .kind = OP_SUB_ALL }; post(&o2, NULL, 0); }
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        note_adv(&ev->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        LOCK(); s_st.scanning = false; UNLOCK();
        evlog_text(EV_SYS, "scan stopped (%d)", ev->disc_complete.reason);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            LOCK();
            s_st.conn = ev->connect.conn_handle;
            s_st.link = DBG_LINK_CONNECTED;
            s_st.connected_ms = now_ms();
            s_st.mtu = BLE_ATT_MTU_DFLT;
            s_st.encrypted = s_st.bonded = s_st.authenticated = false;
            s_gatt.n_chr = s_gatt.n_svc = 0;
            s_gatt.complete = false;
            s_h_settings = 0;
            bool sec = s_st.auto_secure;
            UNLOCK();
            refresh_sec(ev->connect.conn_handle);
            evlog_text(EV_SYS, "connected (conn %u)", ev->connect.conn_handle);
            op_t op = { .kind = OP_MTU };
            post(&op, NULL, 0);
            if (sec) {
                int rc = ble_gap_security_initiate(ev->connect.conn_handle);
                evlog_text(rc && rc != BLE_HS_EALREADY ? EV_ERR : EV_SYS,
                           "security initiate: %d%s", rc,
                           rc ? "" : " (bonded -> encrypt, else pair: camera must be in pairing mode)");
                // Couldn't even start: no ENC_CHANGE will come, so don't hold
                // discovery hostage to it.
                after_secure_or_connect(rc != 0 && rc != BLE_HS_EALREADY);
            } else {
                after_secure_or_connect(false);
            }
        } else {
            LOCK(); s_st.link = DBG_LINK_IDLE; s_st.conn = BLE_HS_CONN_HANDLE_NONE; UNLOCK();
            evlog_text(EV_ERR, "connect failed (status %d)", ev->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        LOCK();
        s_st.conn = BLE_HS_CONN_HANDLE_NONE;
        s_st.link = DBG_LINK_IDLE;
        s_st.mtu = 0;
        s_st.encrypted = s_st.bonded = s_st.authenticated = false;
        s_st.last_disc_reason = ev->disconnect.reason;
        s_h_settings = 0;
        UNLOCK();
        evlog_text(EV_SYS, "disconnected (reason 0x%03x)", ev->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        refresh_sec(ev->enc_change.conn_handle);
        if (ev->enc_change.status == 0) {
            LOCK(); bool b = s_st.bonded; uint8_t ks = s_st.key_size; UNLOCK();
            evlog_text(EV_SYS, "encrypted (bonded=%d, key %u)", b, ks);
            after_secure_or_connect(true);
        } else {
            evlog_text(EV_ERR, "encryption/pairing failed (%d) -- put the camera in pairing mode, "
                       "or the bond on the camera side is gone (delete it here and re-pair)",
                       ev->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        LOCK(); s_st.mtu = ev->mtu.value; UNLOCK();
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.om) {
            uint8_t buf[512];
            uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
            if (len > sizeof(buf)) { len = sizeof(buf); }
            os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
            evlog_push(ev->notify_rx.indication ? EV_IND : EV_RX, ev->notify_rx.attr_handle, 0, 0, buf, len);
            LOCK(); s_st.rx_pkts++; UNLOCK();
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The camera lost our keys (e.g. "reset connections"): drop the stale
        // bond and pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        evlog_text(EV_SYS, "repeat pairing: old bond deleted, re-pairing");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        evlog_text(EV_SYS, "passkey action %d (Just Works expected)", ev->passkey.params.action);
        break;
    default:
        break;
    }
    return 0;
}

esp_err_t dbg_ble_scan(bool on, uint32_t secs, bool active)
{
    LOCK(); bool synced = s_st.synced; UNLOCK();
    if (!synced) { return ESP_ERR_INVALID_STATE; }
    if (!on) {
        ble_gap_disc_cancel();
        LOCK(); s_st.scanning = false; UNLOCK();
        evlog_text(EV_SYS, "scan stopped");
        return ESP_OK;
    }
    if (ble_gap_disc_active()) { ble_gap_disc_cancel(); }
    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_disc_params p = {
        .itvl = 0x0060, .window = 0x0030,      // 50% duty: leaves air time for the Wi-Fi AP
        .passive = active ? 0 : 1, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own, secs ? (int32_t)secs * 1000 : BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) { evlog_text(EV_ERR, "scan start failed (%d)", rc); return ESP_FAIL; }
    LOCK(); s_st.scanning = true; UNLOCK();
    evlog_text(EV_SYS, "scan started (%s, %lus)", active ? "active" : "passive", (unsigned long)secs);
    return ESP_OK;
}

int dbg_ble_devices(dbg_dev_t *out, int max)
{
    LOCK();
    int n = s_dev_n < max ? s_dev_n : max;
    memcpy(out, s_devs, n * sizeof(dbg_dev_t));
    UNLOCK();
    for (int i = 0; i < n; i++) {
        ble_addr_t a = { .type = out[i].addr_type };
        memcpy(a.val, out[i].addr, 6);
        out[i].bonded = addr_bonded(&a);
    }
    return n;
}

void dbg_ble_devices_clear(void) { LOCK(); s_dev_n = 0; UNLOCK(); }

esp_err_t dbg_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    LOCK();
    bool busy = s_st.link != DBG_LINK_IDLE;
    UNLOCK();
    if (busy) { return ESP_ERR_INVALID_STATE; }
    ble_gap_disc_cancel();
    LOCK(); s_st.scanning = false; UNLOCK();
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x0010, .scan_window = 0x0010,
        .itvl_min = 24, .itvl_max = 40, .latency = 0,
        .supervision_timeout = 400,
    };
    int rc = ble_gap_connect(own, &a, 10000, &cp, gap_event, NULL);
    if (rc) { evlog_text(EV_ERR, "connect start failed (%d)", rc); return ESP_FAIL; }
    LOCK();
    s_st.link = DBG_LINK_CONNECTING;
    memcpy(s_st.peer, addr, 6);
    s_st.peer_type = addr_type;
    UNLOCK();
    evlog_text(EV_SYS, "connecting to %02X:%02X:%02X:%02X:%02X:%02X (type %u)%s",
               addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], addr_type,
               addr_bonded(&a) ? " [bonded]" : "");
    return ESP_OK;
}

void dbg_ble_disconnect(void)
{
    LOCK(); uint16_t c = s_st.conn; dbg_link_t l = s_st.link; UNLOCK();
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    } else if (l == DBG_LINK_CONNECTING) {
        ble_gap_conn_cancel();
        LOCK(); s_st.link = DBG_LINK_IDLE; UNLOCK();
        evlog_text(EV_SYS, "connect cancelled");
    }
}

esp_err_t dbg_ble_secure(void)
{
    uint16_t c = conn_handle();
    if (c == BLE_HS_CONN_HANDLE_NONE) { return ESP_ERR_INVALID_STATE; }
    int rc = ble_gap_security_initiate(c);
    evlog_text(rc && rc != BLE_HS_EALREADY ? EV_ERR : EV_SYS, "security initiate: %d", rc);
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}

void dbg_ble_set_auto(bool secure, bool discover, bool subscribe)
{
    LOCK();
    s_st.auto_secure = secure;
    s_st.auto_discover = discover;
    s_st.auto_subscribe = subscribe;
    UNLOCK();
}

void dbg_ble_status(dbg_status_t *out)
{
    LOCK();
    *out = s_st;
    uint16_t c = s_st.conn;
    UNLOCK();
    if (c != BLE_HS_CONN_HANDLE_NONE) { ble_gap_conn_rssi(c, &out->rssi); }
}

void dbg_ble_gatt(dbg_gatt_t *out) { LOCK(); *out = s_gatt; UNLOCK(); }

int dbg_ble_bonds(uint8_t addrs[][6], uint8_t *types, int max)
{
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) { return 0; }
    if (n > max) { n = max; }
    for (int i = 0; i < n; i++) {
        memcpy(addrs[i], peers[i].val, 6);
        types[i] = peers[i].type;
    }
    return n;
}

esp_err_t dbg_ble_delete_bond(const uint8_t addr[6], uint8_t type)
{
    ble_addr_t a = { .type = type };
    memcpy(a.val, addr, 6);
    int rc = ble_store_util_delete_peer(&a);
    evlog_text(rc ? EV_ERR : EV_SYS, "delete bond %02X:%02X:%02X:%02X:%02X:%02X: %d",
               addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t dbg_ble_delete_all_bonds(void)
{
    int rc = ble_store_clear();
    evlog_text(rc ? EV_ERR : EV_SYS, "delete ALL bonds: %d", rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// ---- host lifecycle ----------------------------------------------------
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, a, NULL);
    LOCK(); s_st.synced = true; UNLOCK();
    evlog_text(EV_SYS, "BLE host ready, own addr %02X:%02X:%02X:%02X:%02X:%02X",
               a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void on_reset(int reason)
{
    LOCK(); s_st.synced = false; UNLOCK();
    ESP_LOGW(TAG, "host reset %d", reason);
}

static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }

void dbg_ble_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_opq = xQueueCreate(16, sizeof(op_t));
    // 6 KB: evlog_text -> ESP_LOG -> syslog mirror nests two vsnprintf frames.
    xTaskCreate(gatt_task, "gatt_task", 6144, NULL, 6, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Same security setup as the original firmware: Just-Works, bonding, SC,
    // both key types distributed -- so either image can use the stored bond.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("RecLink");   // same name the camera knows us by
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
