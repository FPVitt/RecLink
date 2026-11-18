// rid.h -- Remote ID receiver (F7, spec 8.8): rid_task, its two inbound message buffers, the
// summary, the single-writer status words, and the reception test.
//
// Ownership (spec 8.8.2 / 8.8.3 / 8.8.8 -- one writer per word, readers never block):
//   rid_task        : the aircraft table, the summary, the decode counters. NEVER calls esp_wifi_*
//                     or NimBLE; it only drains rid_rxq_ble / rid_rxq_wifi.
//   NimBLE host task: rid_ble_ingest() -> rid_rxq_ble (+ BLE rx/drop counters).
//   Wi-Fi task      : rid_wifi_ingest() -> rid_rxq_wifi (+ Wi-Fi rx/drop/mgmt counters).
//   housekeeping    : rid_set_active(), rid_set_ble_ingest(), rid_set_wifi_due_ms(),
//                     rid_test_tick(), rid_tick_1s(); FLAG_RID_FAULT.
//   cam_task        : rid_set_scan_status()  (rid_scan_ok).
//   portal_task     : rid_set_wifi_state()   (rid_wifi_state).
// Everything here is behind "Remote ID active" (spec 8.8.5) and behind the boot-time allocation:
// with rid.enabled = 0 at boot nothing is allocated, the task is never created, and every entry
// point is a safe no-op.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "rid_core.h"
#include "cfg.h"

// ---- lifecycle -----------------------------------------------------------------------------
// Allocate everything (one heap block, never freed) and create rid_task -- ONCE, from app_main
// during init, only if cfg.rid.enabled is 1 at boot. Disabled at boot -> nothing is reserved and
// enabling needs a reboot. Returns rid_available().
bool     rid_boot_init(bool enabled);
bool     rid_available(void);          // allocated at boot and rid_task exists
bool     rid_alloc_failed(void);       // enabled at boot but the allocation failed
bool     rid_running(void);
uint32_t rid_heartbeat_ms(void);       // uptime of rid_task's last loop (1 Hz or better)

// ---- gate + BLE ingest (writer: housekeeping) ------------------------------------------------
void rid_set_active(bool active);
bool rid_active(void);
// The BLE "rid_listen" scan reason AND the gap_event matcher's enable (rid_ble_ingest_enable).
void rid_set_ble_ingest(bool on);
bool rid_ble_listen(void);

static inline bool rid_mode_has_ble(uint8_t mode)  { return mode == RID_MODE_BLE  || mode == RID_MODE_BOTH; }
static inline bool rid_mode_has_wifi(uint8_t mode) { return mode == RID_MODE_WIFI || mode == RID_MODE_BOTH; }

// ---- producers (vendor task context: allocation-free, log-free, bounded) ---------------------
// NimBLE host task, every advertising report: returns true if it was a Remote ID advert (it is
// then NOT forwarded to the camera queue, whether it was accepted or dropped).
bool rid_ble_ingest(const uint8_t addr[6], int8_t rssi, const uint8_t *adv, size_t len);
// Wi-Fi task, every promiscuous management frame (len = sig_len - 4).
void rid_wifi_ingest(const uint8_t *frame, size_t len, int8_t rssi);

// ---- status words -----------------------------------------------------------------------------
typedef enum { RID_WIFI_OFF = 0, RID_WIFI_AP = 1, RID_WIFI_RX = 2, RID_WIFI_TEST = 3 } rid_wifi_state_t;
void    rid_set_wifi_state(uint8_t st);        // portal_task only
uint8_t rid_wifi_state(void);
void     rid_set_wifi_due_ms(uint32_t ms);     // housekeeping: RID_RX due (0 = not due)
uint32_t rid_wifi_due_ms(void);
void rid_set_scan_status(bool running, bool ok);   // cam_task only
bool rid_scan_running(void);
bool rid_scan_ok(void);

enum { RID_FAULT_SUBSYS = 1u << 0, RID_FAULT_BLE = 1u << 1, RID_FAULT_WIFI = 1u << 2 };
// Built at render time from the three single-writer words (never stored, spec 8.8.7). 0 when
// Remote ID is disabled or inactive.
uint8_t rid_fault_mask(const cfg_t *c, uint32_t now);

// ---- the summary (writer: rid_task; portMUX copy-only, 80 bytes) ------------------------------
// The displayed aircraft, one rid_msg_t each, already sorted level-descending then nearest-first
// (spec 8.8.7). osd_task runs rid_seq_select() over it to show ONE aircraft at a time.
typedef struct {
    uint32_t upd_ms;           // when rid_task last computed it
    uint16_t near_dist_m;      // nearest eligible aircraft (portal)
    uint16_t seq;
    uint8_t  count;            // eligible aircraft inside notice_m
    uint8_t  level;            // highest displayed level (NONE when there is no message)
    uint8_t  flags;            // RID_SUM_*
    uint8_t  n_entries;        // table entries in use
    uint8_t  n_msg;            // valid entries in msg[]
    uint8_t  _pad[3];
    rid_msg_t msg[RID_MAX_AC];
} rid_summary_t;
enum { RID_SUM_HAVE = 1u << 0, RID_SUM_HDG = 1u << 1, RID_SUM_NEAR = 1u << 2 };
_Static_assert(sizeof(rid_summary_t) == 16 + 8 * RID_MAX_AC, "the summary is a small fixed copy (spec 8.8.4)");
void rid_summary_get(rid_summary_t *out);

// ---- the table snapshot (portal; 0-timeout take, spec 8.8.8) ---------------------------------
typedef struct {
    rid_ac_t ac[RID_MAX_AC];
    uint32_t snap_ms;
} rid_snapshot_t;
bool rid_table_try_copy(rid_snapshot_t *out);
// The reception test's aircraft list (spec 8.8.10, extended): the live table while the test runs
// (ingested WITHOUT the GPS gate during the test only), frozen after it ends, cleared by the next
// test or rid.enabled = 0. RAM-only, never logged. 0-timeout take: false = busy (or never allocated).
bool rid_test_try_copy(rid_snapshot_t *out);

// ---- counters (each word has one writer) ---------------------------------------------------
typedef struct {
    uint32_t ble_rx, ble_drops;            // NimBLE host task
    uint32_t wifi_rx, wifi_drops, mgmt;    // Wi-Fi task
    uint32_t beacon, nan, decoded;         // rid_task
    uint32_t jump_rejects, range_rejects, evict_drops;
} rid_counters_t;
void     rid_counters_get(rid_counters_t *out);
void     rid_tick_1s(uint32_t now);        // housekeeping: management-frames-in-30-s window
uint32_t rid_wifi_frames_30s(void);

// ---- reception test (spec 8.8.10) ------------------------------------------------------------
enum { RID_TEST_IDLE = 0, RID_TEST_RUN = 1, RID_TEST_DONE = 2 };
typedef struct {
    uint8_t  state;
    bool     wifi;
    bool     aborted;
    uint16_t secs_left;
    uint32_t ble_frames, wifi_frames, beacon, nan;
    uint8_t  aircraft;                     // distinct transmitters heard
} rid_test_status_t;
bool rid_test_start(uint16_t secs, bool wifi, uint32_t now);   // false if one is running
void rid_test_abort(void);
void rid_test_tick(uint32_t now);          // housekeeping: expiry
bool rid_test_active(void);
bool rid_test_wifi(void);                  // running and includes Wi-Fi
uint32_t rid_test_start_ms(void);
void rid_test_status(rid_test_status_t *out, uint32_t now);

// cfg.rid -> the pure logic's parameters (points into *c; valid while c is).
void rid_params_from_cfg(const cfg_t *c, rid_params_t *p);
