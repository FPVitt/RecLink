// rid_task.c -- the Remote ID receiver task, its two inbound message buffers, the summary, the
// single-writer status words and the reception test (spec 8.8.3 / 8.8.4 / 8.8.8 / 8.8.10).
//
// rid_task runs at priority 2 -- BELOW housekeeping (3) -- and is deliberately NOT on the Task
// WDT (spec 8.8.13 #3): a CPU-bound hang here (a decode loop, an eviction loop) can never starve
// the task that detects it, and it disables the feature (FLAG_RID_FAULT, OSD "RID RX FAIL")
// instead of rebooting the aircraft's bridge. It never calls esp_wifi_* or NimBLE.
//
// Privacy (spec 8.8.4 / 13.10): nothing received is ever logged or stored. The only log lines in
// this file carry counters.
//
// Memory: NOTHING sizeable is reserved statically. rid_boot_init() allocates one block (task
// stack + TCB, both message buffers, the table + its snapshot, the work items) ONCE, during init
// and before steady state, and only if cfg.rid.enabled was 1 at boot. Never freed (a later 1 -> 0
// only stops all activity; the memory returns at the next reboot). With it never allocated, every
// entry point below is a safe no-op. Only a few hundred bytes of scalars (flags, counters, the
// 16-byte summary) are static.
#include "rid.h"

#include <string.h>

#include "fc_state.h"
#include "monoclock.h"
#include "platform.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"

static const char *TAG = "rid";

// ---- tuning -------------------------------------------------------------------------------------
#define RID_TASK_PRIO        2           // below housekeeping (3), spec 8.8.8
// rid_task stack. Static analysis (gcc -fstack-usage on this build + the libm frames disassembled,
// 2026-11-18): the deepest chain is rid_task 144 -> rid_table_ingest_pack 112 ->
// rid_table_ingest_msg (ingest_location inlined) 240 -> rid_geo 48 -> cosf 32 ->
// __ieee754_rem_pio2f 64 -> __kernel_rem_pio2f 448 -> scalbnf/floorf + soft-float ~64 = ~1150 B;
// the decoders are 16/32 B and rid_table_update's branch is shallower. + FreeRTOS kernel calls and
// the RISC-V context frame on preemption (~450 B) = ~1.6 KB worst case. No logging on this task.
// 2560 = ~60 % margin. Tune after hardware: the "stack free" log (housekeeping) reports rid_task.
#ifndef RID_TASK_STACK
#define RID_TASK_STACK       2560
#endif
#define RID_WAIT_MS          500         // ulTaskNotifyTake timeout == the 2 Hz aging/level tick
#define RID_TICK_MS          500
#define RID_UPDATE_MIN_MS    100         // table pass at most 10 Hz under a flood (latency <= 100 ms)
// Bounded work per wake (spec 8.8.8); each message buffer holds exactly one wake's batch of
// worst-case items (rid_task drains at least that much per wake, and the producers are capped at
// 64 accepted/s each), with a floor of 4 items.
#ifndef RID_BLE_PER_WAKE
#define RID_BLE_PER_WAKE     16
#endif
#ifndef RID_WIFI_PER_WAKE
#define RID_WIFI_PER_WAKE    4
#endif

// ---- inbound messages (single writer each; drop-newest, counted) ------------------------------
typedef struct __attribute__((packed)) {
    uint8_t addr[6];
    uint8_t msg[RID_MSG_SIZE];
} ble_item_t;                                                 // 31 B (+4 B buffer header); no RSSI kept

typedef struct __attribute__((packed)) {
    uint8_t src[6];
    uint8_t kind;                                             // RID_WIFI_KIND_*
    uint8_t len;                                              // pack bytes that follow (<= 228)
    uint8_t pack[RID_PACK_BYTES_MAX];
} wifi_item_t;
#define WIFI_ITEM_HDR offsetof(wifi_item_t, pack)
_Static_assert(RID_PACK_BYTES_MAX <= 255, "pack length fits the u8 len");

// A message buffer stores each message behind a size_t length word.
#define RXQ_MSG_OVERHEAD     sizeof(size_t)
#define RXQ_BLE_BYTES        (RID_BLE_PER_WAKE * (sizeof(ble_item_t) + RXQ_MSG_OVERHEAD))    // 560
#define RXQ_WIFI_BYTES       (RID_WIFI_PER_WAKE * (sizeof(wifi_item_t) + RXQ_MSG_OVERHEAD))  // 960
_Static_assert(RID_BLE_PER_WAKE >= 4 && RID_WIFI_PER_WAKE >= 4, "floor: a few items per buffer");

// ---- everything sizeable: one block, allocated once at boot iff rid.enabled -------------------
typedef struct {
    StaticTask_t          tcb;
    StackType_t           stack[RID_TASK_STACK / sizeof(StackType_t)];
    StaticMessageBuffer_t bleq_s, wifiq_s;
    uint8_t               bleq_buf[RXQ_BLE_BYTES + 1];
    uint8_t               wifiq_buf[RXQ_WIFI_BYTES + 1];
    StaticSemaphore_t     snap_mux_buf;
    rid_table_t           tab;           // rid_task's private working copy
    rid_snapshot_t        snap;          // published under snap_mux (copy only)
    rid_snapshot_t        test_snap;     // the reception test's aircraft: live while it runs,
                                         // frozen after it ends (until the next test / rid off)
    ble_item_t            bi;            // rid_task work items (off its stack)
    wifi_item_t           wi;
    wifi_item_t           wi_cb;         // the Wi-Fi callback's item (its only writer)
    fc_state_t            fc;
    uint8_t               t_addr[RID_MAX_AC][7];   // distinct test transmitters (never leave here)
    uint32_t              mgmt_ring[31];           // 30 s management-frame window
    rid_lvl_state_t       lvl[RID_MAX_AC];         // each aircraft's own displayed level
    uint32_t              lvl_born[RID_MAX_AC];    // first_seen_ms the state belongs to (slot reuse)
} rid_mem_t;

static rid_mem_t *s_mem;                 // NULL = Remote ID not enabled at boot (or alloc failed)
static bool s_alloc_failed;
static MessageBufferHandle_t s_bleq, s_wifiq;

// ---- task --------------------------------------------------------------------------------------
static TaskHandle_t volatile s_task;
static volatile uint32_t s_hb_ms;

// ---- single-writer words -----------------------------------------------------------------------
static volatile bool     s_active;       // housekeeping
static volatile bool     s_ble_ingest;   // housekeeping (rid_ble_ingest_enable)
static volatile uint8_t  s_wifi_state;   // portal_task
static volatile uint32_t s_wifi_due_ms;  // housekeeping
static volatile bool     s_scan_running; // cam_task
static volatile bool     s_scan_ok = true;   // cam_task

// counters: NimBLE host task
static volatile uint32_t s_ble_rx, s_ble_drops, s_ble_win_s, s_ble_win_n;
// counters: Wi-Fi task
static volatile uint32_t s_wifi_rx, s_wifi_drops, s_mgmt, s_wifi_win_s, s_wifi_win_n;
// counters: rid_task
static volatile uint32_t s_beacon, s_nan, s_decoded;
static volatile uint32_t s_jump_rejects, s_range_rejects, s_evict_drops;

// ---- the table + snapshot live in s_mem ----------------------------------------------------
#define s_tab  (s_mem->tab)
#define s_snap (s_mem->snap)
static SemaphoreHandle_t s_snap_mux;

// ---- the 16-byte summary (spinlock, copy only: a hung rid_task cannot hold it) ----------------
static rid_summary_t s_sum;
static portMUX_TYPE  s_sum_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- reception test ---------------------------------------------------------------------------
static portMUX_TYPE s_test_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t  state;
    bool     wifi, aborted;
    uint16_t gen;
    uint32_t start_ms, until_ms;
} s_test;
// written by rid_task only (reset when it sees a new test generation)
static volatile uint32_t s_t_ble, s_t_wifi, s_t_beacon, s_t_nan;
static volatile uint8_t  s_t_ac;

// ---- 30 s management-frame window (housekeeping; ring in s_mem) ------------------------------
static uint8_t  s_mgmt_head;
static volatile uint32_t s_mgmt_30s;

// ================================================================================================
// small accessors
// ================================================================================================
bool     rid_running(void)          { return s_task != NULL; }
bool     rid_available(void)        { return s_mem != NULL && s_task != NULL; }
bool     rid_alloc_failed(void)     { return s_alloc_failed; }
uint32_t rid_heartbeat_ms(void)     { return s_hb_ms; }
void     rid_set_active(bool a)     { s_active = a; }
bool     rid_active(void)           { return s_active; }
void     rid_set_ble_ingest(bool on){ s_ble_ingest = on; }
bool     rid_ble_listen(void)       { return s_ble_ingest; }
void     rid_set_wifi_state(uint8_t st) { s_wifi_state = st; }
uint8_t  rid_wifi_state(void)       { return s_wifi_state; }
void     rid_set_wifi_due_ms(uint32_t ms) { s_wifi_due_ms = ms; }
uint32_t rid_wifi_due_ms(void)      { return s_wifi_due_ms; }
void     rid_set_scan_status(bool running, bool ok) { s_scan_running = running; s_scan_ok = ok; }
bool     rid_scan_running(void)     { return s_scan_running; }
bool     rid_scan_ok(void)          { return s_scan_ok; }
uint32_t rid_wifi_frames_30s(void)  { return s_mgmt_30s; }

void rid_params_from_cfg(const cfg_t *c, rid_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->notice_m = c->rid.warn.notice_m;
    p->warn_m   = c->rid.warn.warn_m;
    p->alert_m  = c->rid.warn.alert_m;
    p->stale_s  = c->rid.stale_s;
    p->ground   = c->rid.warn.ground;
    p->ignore   = (const char (*)[RID_ID_SIZE + 1])c->rid.ignore;
    p->n_ignore = RID_IGNORE_MAX;
}

uint8_t rid_fault_mask(const cfg_t *c, uint32_t now)
{
    if (!c || !c->rid.enabled || !s_active) { return 0; }   // inactive: nothing, not even FAIL
    uint8_t m = 0;
    if (bridge_flag_get(FLAG_RID_FAULT)) { m |= RID_FAULT_SUBSYS; }
    if (rid_mode_has_ble(c->rid.mode) && !s_scan_ok) { m |= RID_FAULT_BLE; }
    if (rid_mode_has_wifi(c->rid.mode) && flight_may_be_airborne()) {
        uint32_t due = s_wifi_due_ms;
        if (due && (now - due) >= 3000u && s_wifi_state != RID_WIFI_RX) { m |= RID_FAULT_WIFI; }
    }
    return m;
}

void rid_summary_get(rid_summary_t *out)
{
    portENTER_CRITICAL(&s_sum_mux);
    *out = s_sum;                                        // 16 bytes, nothing else
    portEXIT_CRITICAL(&s_sum_mux);
}

static void summary_put(const rid_summary_t *in)
{
    portENTER_CRITICAL(&s_sum_mux);
    s_sum = *in;
    portEXIT_CRITICAL(&s_sum_mux);
}

bool rid_test_try_copy(rid_snapshot_t *out)
{
    if (!s_snap_mux || xSemaphoreTake(s_snap_mux, 0) != pdTRUE) { return false; }   // never wait
    *out = s_mem->test_snap;
    xSemaphoreGive(s_snap_mux);
    return true;
}

bool rid_table_try_copy(rid_snapshot_t *out)
{
    if (!s_snap_mux || xSemaphoreTake(s_snap_mux, 0) != pdTRUE) { return false; }   // never wait
    *out = s_snap;
    xSemaphoreGive(s_snap_mux);
    return true;
}

void rid_counters_get(rid_counters_t *o)
{
    o->ble_rx = s_ble_rx;     o->ble_drops = s_ble_drops;
    o->wifi_rx = s_wifi_rx;   o->wifi_drops = s_wifi_drops;   o->mgmt = s_mgmt;
    o->beacon = s_beacon;     o->nan = s_nan;                 o->decoded = s_decoded;
    o->jump_rejects = s_jump_rejects; o->range_rejects = s_range_rejects;
    o->evict_drops = s_evict_drops;
}

void rid_tick_1s(uint32_t now)
{
    (void)now;
    if (!s_mem) { return; }
    uint32_t total = s_mgmt;
    s_mem->mgmt_ring[s_mgmt_head] = total;
    uint8_t oldest = (uint8_t)((s_mgmt_head + 1) % 31);     // the sample 30 s ago
    s_mgmt_30s = total - s_mem->mgmt_ring[oldest];
    s_mgmt_head = oldest;
}

// ================================================================================================
// producers -- vendor task context. Allocation-free, log-free, bounded; one writer per counter.
// ================================================================================================
bool rid_ble_ingest(const uint8_t addr[6], int8_t rssi, const uint8_t *adv, size_t len)
{
    (void)rssi;                                 // not kept per aircraft (data minimisation)
    if (!s_ble_ingest || !s_task || !s_bleq) { return false; }
    const uint8_t *msg = NULL;
    if (!rid_ble_match(adv, len, &msg)) { return false; }
    // A Remote ID advert is never a camera advert: from here on it is diverted, never forwarded.
    uint32_t sec = now_ms() / 1000u;
    if (sec != s_ble_win_s) { s_ble_win_s = sec; s_ble_win_n = 0; }
    if (s_ble_win_n >= RID_ACCEPT_PER_S) { s_ble_drops++; return true; }   // 64 matches/s cap
    s_ble_win_n++;
    ble_item_t it;
    memcpy(it.addr, addr, 6);
    memcpy(it.msg, msg, RID_MSG_SIZE);
    if (xMessageBufferSend(s_bleq, &it, sizeof(it), 0) != sizeof(it)) { s_ble_drops++; return true; }
    s_ble_rx++;
    TaskHandle_t t = s_task;
    if (t) { xTaskNotifyGive(t); }
    return true;
}

void rid_wifi_ingest(const uint8_t *frame, size_t len, int8_t rssi)
{
    (void)rssi;                                 // not kept per aircraft (data minimisation)
    s_mgmt++;                                   // "management frames received" health counter
    if (!s_task || !s_wifiq || !s_mem) { return; }
    uint8_t src[6];
    const uint8_t *pack = NULL;
    size_t plen = 0;
    int kind = rid_wifi_match(frame, len, src, &pack, &plen);
    if (kind == RID_WIFI_KIND_NONE || plen > RID_PACK_BYTES_MAX) { return; }
    uint32_t sec = now_ms() / 1000u;
    if (sec != s_wifi_win_s) { s_wifi_win_s = sec; s_wifi_win_n = 0; }
    if (s_wifi_win_n >= RID_ACCEPT_PER_S) { s_wifi_drops++; return; }      // 64/s cap, as BLE
    s_wifi_win_n++;
    // Built straight into the callback's own item in s_mem (the Wi-Fi task is its only writer)
    // -- the callback stack is the driver's, keep it small.
    wifi_item_t *it = &s_mem->wi_cb;
    memcpy(it->src, src, 6);
    it->kind = (uint8_t)kind;
    it->len  = (uint8_t)plen;
    memcpy(it->pack, pack, plen);
    size_t n = WIFI_ITEM_HDR + plen;
    if (xMessageBufferSend(s_wifiq, it, n, 0) != n) { s_wifi_drops++; return; }
    s_wifi_rx++;
    TaskHandle_t t = s_task;
    if (t) { xTaskNotifyGive(t); }
}

// ================================================================================================
// reception test
// ================================================================================================
bool rid_test_start(uint16_t secs, bool wifi, uint32_t now)
{
    bool ok = false;
    if (!rid_available()) { return false; }               // needs the boot-time allocation
    portENTER_CRITICAL(&s_test_mux);
    if (s_test.state != RID_TEST_RUN) {
        s_test.state = RID_TEST_RUN;
        s_test.wifi = wifi;
        s_test.aborted = false;
        s_test.gen++;
        s_test.start_ms = now;
        s_test.until_ms = now + (uint32_t)secs * 1000u;
        ok = true;
    }
    portEXIT_CRITICAL(&s_test_mux);
    TaskHandle_t t = s_task;
    if (ok && t) { xTaskNotifyGive(t); }
    return ok;
}

void rid_test_abort(void)
{
    portENTER_CRITICAL(&s_test_mux);
    if (s_test.state == RID_TEST_RUN) { s_test.state = RID_TEST_DONE; s_test.aborted = true; }
    portEXIT_CRITICAL(&s_test_mux);
}

void rid_test_tick(uint32_t now)
{
    portENTER_CRITICAL(&s_test_mux);
    if (s_test.state == RID_TEST_RUN && (int32_t)(now - s_test.until_ms) >= 0) { s_test.state = RID_TEST_DONE; }
    portEXIT_CRITICAL(&s_test_mux);
}

bool rid_test_active(void) { return s_test.state == RID_TEST_RUN; }
bool rid_test_wifi(void)   { return s_test.state == RID_TEST_RUN && s_test.wifi; }
uint32_t rid_test_start_ms(void) { return s_test.start_ms; }

void rid_test_status(rid_test_status_t *o, uint32_t now)
{
    memset(o, 0, sizeof(*o));
    portENTER_CRITICAL(&s_test_mux);
    o->state = s_test.state;
    o->wifi = s_test.wifi;
    o->aborted = s_test.aborted;
    uint32_t until = s_test.until_ms;
    portEXIT_CRITICAL(&s_test_mux);
    if (o->state == RID_TEST_RUN && (int32_t)(until - now) > 0) {
        o->secs_left = (uint16_t)((until - now + 999u) / 1000u);
    }
    if (o->state != RID_TEST_IDLE) {
        o->ble_frames = s_t_ble; o->wifi_frames = s_t_wifi;
        o->beacon = s_t_beacon;  o->nan = s_t_nan;   o->aircraft = s_t_ac;
    }
}

// ================================================================================================
// rid_task
// ================================================================================================
// Distinct transmitters heard during a test (counters only -- the addresses never leave here).
#define s_t_addr (s_mem->t_addr)
static void test_note_src(uint8_t tr, const uint8_t addr[6])
{
    uint8_t n = s_t_ac;
    for (uint8_t i = 0; i < n && i < RID_MAX_AC; i++) {
        if (s_t_addr[i][0] == tr && memcmp(&s_t_addr[i][1], addr, 6) == 0) { return; }
    }
    if (n < RID_MAX_AC) { s_t_addr[n][0] = tr; memcpy(&s_t_addr[n][1], addr, 6); }
    if (n < 255) { s_t_ac = (uint8_t)(n + 1); }
}

static void publish_test(uint32_t now, bool clear)
{
    if (xSemaphoreTake(s_snap_mux, pdMS_TO_TICKS(5)) != pdTRUE) { return; }
    if (clear) { memset(&s_mem->test_snap, 0, sizeof(s_mem->test_snap)); }
    else       { memcpy(s_mem->test_snap.ac, s_tab.ac, sizeof(s_mem->test_snap.ac)); }
    s_mem->test_snap.snap_ms = now;
    xSemaphoreGive(s_snap_mux);
}

static void publish_snapshot(uint32_t now)
{
    if (xSemaphoreTake(s_snap_mux, pdMS_TO_TICKS(5)) != pdTRUE) { return; }   // next pass retries
    memcpy(s_snap.ac, s_tab.ac, sizeof(s_snap.ac));
    s_snap.snap_ms = now;
    xSemaphoreGive(s_snap_mux);
}

static void rid_task(void *arg)
{
    (void)arg;
    // Work items live in s_mem: keeps the small stack for the decode path.
    ble_item_t  *const pbi = &s_mem->bi;
    wifi_item_t *const pwi = &s_mem->wi;
    #define bi (*pbi)
    #define wi (*pwi)
    #define fc (s_mem->fc)
    uint32_t last_tick = 0;
    uint16_t seen_gen = 0;
    bool cleared = true;
    bool test_cleared = true;                                // test_snap is empty
    bool dirty = false;                                      // ingested since the last table pass
    uint16_t seq = 0;

    for (;;) {
        s_hb_ms = now_ms();                                  // heartbeat (housekeeping checks it)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(dirty ? RID_UPDATE_MIN_MS : RID_WAIT_MS));
        uint32_t now = now_ms();
        s_hb_ms = now;

        bool active = s_active && !bridge_flag_get(FLAG_RID_FAULT);
        bool test = rid_test_active();
        const cfg_t *c = cfg_get();
        if (s_test.gen != seen_gen) {                        // a new test: fresh counters + list
            seen_gen = s_test.gen;
            s_t_ble = s_t_wifi = s_t_beacon = s_t_nan = 0;
            s_t_ac = 0;
            memset(s_t_addr, 0, sizeof(s_t_addr));
            if (!active) { rid_table_clear(&s_tab); }        // inactive: the list is the test's own
            publish_test(now, true);
            test_cleared = true;
        }
        if (!c->rid.enabled && !test_cleared) {              // Remote ID turned off: drop the list
            publish_test(now, true);
            test_cleared = true;
        }
        // The reception test ingests WITHOUT the GPS gate (spec 8.8.10: it must work indoors);
        // outside it the gate is required as always.
        bool ingest = active || (test && c->rid.enabled);

        rid_params_t prm;
        rid_params_from_cfg(c, &prm);
        rid_own_t own = { 0 };
        if (ingest) {
            // dist / brg only with a CONFIRMED own position (the same GPS predicate as the gate).
            fc_state_get(&fc);
            own.ok = rid_gps_ok_eval(fc.link_up, fc.gps_present, fc.gps.fix, fc.gps.sats,
                                     c->rid.min_sats, fc.gps_rx_ms, now);
            own.lat_1e7 = fc.gps.lat_1e7;
            own.lon_1e7 = fc.gps.lon_1e7;
            // Own height above the arming point (MSP_ALTITUDE, armed, <= 1 s old -- msp_task).
            own.alt_ok = fc.alt_valid;
            own.alt_cm = fc.alt_rel_cm;
        }

        // ---- drain, bounded ------------------------------------------------------------------
        int work = 0;
        for (int k = 0; k < RID_BLE_PER_WAKE; k++) {
            size_t n = xMessageBufferReceive(s_bleq, &bi, sizeof(bi), 0);
            if (n != sizeof(bi)) { break; }
            work++;
            if (test) { s_t_ble++; test_note_src(RID_TR_BLE, bi.addr); }
            if (ingest && rid_table_ingest_msg(&s_tab, RID_TR_BLE, bi.addr, bi.msg,
                                               &own, now) >= 0) { s_decoded++; }
        }
        for (int k = 0; k < RID_WIFI_PER_WAKE; k++) {
            size_t n = xMessageBufferReceive(s_wifiq, &wi, sizeof(wi), 0);
            if (n < WIFI_ITEM_HDR) { break; }
            work++;
            size_t plen = n - WIFI_ITEM_HDR;
            if (plen > wi.len) { plen = wi.len; }
            if (wi.kind == RID_WIFI_KIND_BEACON) { s_beacon++; } else if (wi.kind == RID_WIFI_KIND_NAN) { s_nan++; }
            if (test) {
                s_t_wifi++;
                if (wi.kind == RID_WIFI_KIND_BEACON) { s_t_beacon++; } else { s_t_nan++; }
                test_note_src(RID_TR_WIFI, wi.src);
            }
            if (ingest) {
                int d = rid_table_ingest_pack(&s_tab, wi.src, wi.pack, plen, &own, now);
                if (d > 0) { s_decoded += (uint32_t)d; }
            }
        }

        // ---- table -> pick -> level -> summary ---------------------------------------------
        if (!active && ingest) {
            // Test without the gate: keep the list fresh (dist/brg if own position confirmed), but
            // no levels, no summary, no OSD -- Remote ID is not active.
            if ((work > 0 && now - last_tick >= RID_UPDATE_MIN_MS) || now - last_tick >= RID_TICK_MS) {
                last_tick = now;
                cleared = false;
                rid_pick_t pk;
                rid_table_update(&s_tab, &prm, &own, now, &pk);
                publish_test(now, false);
                test_cleared = false;
            }
            dirty = false;
        } else if (!active) {
            if (!cleared) {                                  // inactive: nothing is kept
                rid_table_clear(&s_tab);
                memset(s_mem->lvl, 0, sizeof(s_mem->lvl));
                memset(s_mem->lvl_born, 0, sizeof(s_mem->lvl_born));
                rid_summary_t z = { .upd_ms = now, .seq = ++seq };
                summary_put(&z);
                publish_snapshot(now);
                cleared = true;
            }
            dirty = false;
        } else if (work > 0 && now - last_tick < RID_UPDATE_MIN_MS) {
            dirty = true;                                    // next pass within <= 100 ms
        } else if (work > 0 || dirty || now - last_tick >= RID_TICK_MS) {
            cleared = false;
            dirty = false;
            last_tick = now;
            rid_pick_t pk;
            rid_table_update(&s_tab, &prm, &own, now, &pk);
            s_jump_rejects = s_tab.jump_rejects;
            s_range_rejects = s_tab.range_rejects;
            s_evict_drops = s_tab.evict_drops;

            // Each aircraft has its own displayed level (hysteresis, two-sample ALERT rule); the OSD
            // talks about ONE aircraft at a time, so publish every displayed aircraft, sorted
            // level-descending then nearest-first (insertion sort, <= 8 entries).
            rid_summary_t sm = { 0 };
            sm.upd_ms = now;
            sm.count = pk.count;
            for (int i = 0; i < RID_MAX_AC; i++) {
                const rid_ac_t *a = &s_tab.ac[i];
                if (!a->used) { memset(&s_mem->lvl[i], 0, sizeof(s_mem->lvl[i])); s_mem->lvl_born[i] = 0; continue; }
                if (s_mem->lvl_born[i] != a->first_seen_ms) {      // the slot now holds another aircraft
                    memset(&s_mem->lvl[i], 0, sizeof(s_mem->lvl[i]));
                    s_mem->lvl_born[i] = a->first_seen_ms;
                }
                uint8_t dl = rid_entry_level_step(&s_mem->lvl[i], a, &prm, fc.gps_rx_ms, now);
                if (dl < RID_LVL_NOTICE || !a->dist_valid) { continue; }
                rid_msg_t m = { .dist_m = (uint16_t)(a->dist_m > 65535u ? 65535u : a->dist_m),
                                .brg_deg = a->brg_deg, .level = dl, .vert = a->vert,
                                .id = (uint8_t)(i + 1) };
                int j = sm.n_msg++;
                while (j > 0 && (sm.msg[j - 1].level < m.level ||
                                 (sm.msg[j - 1].level == m.level && sm.msg[j - 1].dist_m > m.dist_m))) {
                    sm.msg[j] = sm.msg[j - 1];
                    j--;
                }
                sm.msg[j] = m;
            }
            if (sm.n_msg) { sm.flags |= RID_SUM_HAVE; sm.level = sm.msg[0].level; }
            if (pk.near_have) {
                sm.flags |= RID_SUM_NEAR;
                sm.near_dist_m = (uint16_t)(pk.near_dist_m > 65535u ? 65535u : pk.near_dist_m);
            }
            if (fc.att_valid && fc.hdg_aligned) { sm.flags |= RID_SUM_HDG; }
            uint8_t used = 0;
            for (int i = 0; i < RID_MAX_AC; i++) { if (s_tab.ac[i].used) { used++; } }
            sm.n_entries = used;
            sm.seq = ++seq;
            summary_put(&sm);
            publish_snapshot(now);
            if (test) { publish_test(now, false); test_cleared = false; }
        }

        // More waiting than one wake may take: yield a tick, then carry on (bounded per wake).
        if (!xMessageBufferIsEmpty(s_bleq) || !xMessageBufferIsEmpty(s_wifiq)) {
            vTaskDelay(1);
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

#undef bi
#undef wi
#undef fc

bool rid_boot_init(bool enabled)
{
    if (s_mem || !enabled) { return s_mem != NULL; }       // disabled at boot: nothing reserved
    rid_mem_t *m = heap_caps_calloc(1, sizeof(rid_mem_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!m) {
        s_alloc_failed = true;                               // fail quiet: the feature stays off
        ESP_LOGE(TAG, "Remote ID: %u B allocation failed -- receiver off", (unsigned)sizeof(rid_mem_t));
        return false;
    }
    s_snap_mux = xSemaphoreCreateMutexStatic(&m->snap_mux_buf);
    s_bleq  = xMessageBufferCreateStatic(RXQ_BLE_BYTES, m->bleq_buf, &m->bleq_s);
    s_wifiq = xMessageBufferCreateStatic(RXQ_WIFI_BYTES, m->wifiq_buf, &m->wifiq_s);
    rid_table_clear(&m->tab);
    s_mem = m;
    s_hb_ms = now_ms();
    // Core 0 on the dual-core S3 (with the app tasks); not subscribed to the Task WDT.
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(rid_task, "rid_task",
                                                   sizeof(m->stack) / sizeof(m->stack[0]), NULL,
                                                   RID_TASK_PRIO, m->stack, &m->tcb, 0);
    s_task = t;
    ESP_LOGI(TAG, "Remote ID: %u B allocated at boot (stack %u, rx %u + %u), rid_task started",
             (unsigned)sizeof(rid_mem_t), (unsigned)RID_TASK_STACK, (unsigned)RXQ_BLE_BYTES,
             (unsigned)RXQ_WIFI_BYTES);
    return true;
}
