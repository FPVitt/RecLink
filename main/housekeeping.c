// housekeeping.c -- lowest-priority task (spec 3.3): LED, button, WDT feed,
// stats flush, camera liveness check, portal supervision, flag upkeep.
#include "applog.h"
#include "monoclock.h"
#include "platform.h"
#include "cfg.h"
#include "msp.h"
#include "fc_state.h"
#include "camera.h"
#include "control.h"
#include "portal.h"
#include "rid.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hk";

// Stack headroom per task (bytes never used). cam_state_t / ctrl_ctx_t growth
// silently eats these; a small number here is the early warning. Logged at the
// 60 s baseline and every 2 min after, so a Wi-Fi client / OTA excursion shows up.
static void log_stacks(void)
{
    static const char *const names[] = { "housekeeping", "osd_task", "ctrl_task",
                                         "cam_task", "msp_task", "portal_task", "rid_task" };
    char sb[160]; size_t o = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t th = xTaskGetHandle(names[i]);
        if (th && o < sizeof(sb)) {
            o += snprintf(sb + o, sizeof(sb) - o, " %s=%u", names[i],
                          (unsigned)uxTaskGetStackHighWaterMark(th));
        }
    }
    ESP_LOGI(TAG, "stack free (low-water B):%s", sb);
}

// spec 12.1: after 60 s uptime the system is in steady state -- no code path
// outside the vendor stacks should still be allocating. Latch the heap
// low-water mark at that point and warn if it keeps sinking (a slow leak).
// Remote ID (spec 8.8.11 test 6): Wi-Fi RID_RX takes the driver's heap at arming, so the
// baseline is re-taken after RID_RX starts -- otherwise every arming would log a false leak.
static void heap_watch(bool rebaseline)
{
    static uint32_t baseline_lwm;      // 0 until latched at 60 s
    static uint32_t warned_at;
    static uint32_t last_log_ms;

    uint32_t up = now_ms();
    uint32_t lwm = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    if (rebaseline && baseline_lwm) {
        baseline_lwm = warned_at = lwm;
        ESP_LOGI(TAG, "heap low-water re-baselined after RID_RX start: %lu B (free %lu B)",
                 (unsigned long)lwm, (unsigned long)esp_get_free_heap_size());
    }

    if (!baseline_lwm) {
        if (up >= 60000) {
            baseline_lwm = warned_at = lwm;
            ESP_LOGI(TAG, "steady-state heap low-water baseline: %lu B", (unsigned long)lwm);
            log_stacks();
        }
        return;
    }
    static uint32_t last_stack_ms;
    if (up - last_stack_ms >= 120000) { last_stack_ms = up; log_stacks(); }
    // Report each fresh 4 KB of regression, at most once every 10 s.
    if (lwm + 4096 < warned_at && up - last_log_ms > 10000) {
        last_log_ms = up;
        warned_at = lwm;
        applog_note("heap low-water regressed: %lu B (baseline %lu) -- possible post-init alloc/leak",
                    (unsigned long)lwm, (unsigned long)baseline_lwm);
    }
}

static led_pattern_t pick_led(const fc_state_t *fc, cam_link_state_t link, tribool_t recording)
{
    // A watchdog-recovery boot flashes LED_FAULT until roughly the point the
    // link would normally be up; the sticky flag stays until acked in the
    // portal. Kept short so it doesn't read as a dead board.
    if (bridge_flag_get(FLAG_WDT_RECOVERED) && now_ms() < 3000) { return LED_FAULT; }
    if (recording == TRIBOOL_TRUE) { return LED_RECORDING; }
    if (!fc->link_up)                  { return LED_FC_DOWN; }
    switch (link) {
    case CAM_LINK_READY: return LED_CAM_IDLE;
    case CAM_LINK_LOST:  return LED_CAM_LOST;
    case CAM_LINK_IDLE:  return LED_CAM_IDLE;
    default:             return LED_CAM_SEARCHING;
    }
}

// ---- Remote ID supervision (spec 8.8.2 / 8.8.5 / 8.8.8) ----------------------------------------
// housekeeping owns the activation gate, the BLE-ingest enable, the RID_RX start / hold / stop
// conditions, the reception-test expiry and the rid_task heartbeat check. Everything is behind
// BOTH rid.enabled (live: 1 -> 0 stops all activity at once) and rid_available() (the boot-time
// allocation: 0 -> 1 needs a reboot). Not allocated: no task, no scan change, no Wi-Fi request,
// no MSP_ATTITUDE.
typedef struct {
    rid_gate_t gate;
    uint32_t   first_arm_ms;        // first decoded armed of this armed episode (0 = none)
    uint32_t   disarm_since;        // decoded disarm held since (0 = not)
    bool       rx_req;
    bool       ble_listen_pub;
    uint8_t    mode_pub, duty_pub;
    uint8_t    wifi_state_prev;
    uint32_t   last_1s;
} rid_sup_t;

static bool rid_supervise(rid_sup_t *r, const fc_state_t *fc, const cfg_t *c, uint32_t now)
{
    bool rebaseline = false;
    bool avail = rid_available();                            // allocated at boot
    bool en = c->rid.enabled && avail;

    // Reception test: expiry, and abort at once on arming (spec 8.8.2.1 #6).
    rid_test_tick(now);
    if (rid_test_active() && (fc->armed || flight_may_be_airborne() || !en)) {
        rid_test_abort();
        portal_wifi_kick();
        applog_note("Remote ID reception test aborted (%s)", en ? "armed" : "disabled");
    }

    // GPS gate: 5 s settle, 15 s loss grace.
    bool gps_ok = rid_gps_ok_eval(fc->link_up, fc->gps_present, fc->gps.fix, fc->gps.sats,
                                  c->rid.min_sats, fc->gps_rx_ms, now);
    bool active = rid_gate_step(&r->gate, en, gps_ok, now);
    rid_set_active(active);
    msp_set_attitude_poll(active);                           // msp_task adds "and armed"

    // rid_task heartbeat (spec 8.8.8): stale > 5 s -> FLAG_RID_FAULT (latched until reboot),
    // Wi-Fi RX off, BLE ingest off, RID scan dropped. No reboot.
    bool fault = bridge_flag_get(FLAG_RID_FAULT);
    if (avail && !fault && (now - rid_heartbeat_ms()) > 5000u) {
        bridge_flag_set(FLAG_RID_FAULT);
        fault = true;
        applog_note("Remote ID task stalled -> receiver disabled until reboot (RID RX FAIL)");
    }

    // BLE listening (the rid_listen scan reason == the gap_event matcher's enable).
    bool ble = en && !fault &&
               ((active && rid_mode_has_ble(c->rid.mode)) || rid_test_active());
    rid_set_ble_ingest(ble);
    if (ble != r->ble_listen_pub || (ble && (c->rid.mode != r->mode_pub || c->rid.ble_duty != r->duty_pub))) {
        r->ble_listen_pub = ble;
        r->mode_pub = c->rid.mode;
        r->duty_pub = c->rid.ble_duty;
        cam_rid_scan_update();                               // explicit re-apply (spec 8.8.3)
    }

    // RID_RX (spec 8.8.2.1): starts only with the FC link up, >= 2 s after the first decoded arm,
    // after F1 has settled and after the AP teardown (portal_task sequences that itself); stops
    // only after a decoded disarm held >= 3 s and !flight_may_be_airborne(), or when Remote ID
    // goes inactive / faults. An FC link drop while armed holds the state (no driver churn).
    if (fc->link_up && fc->armed_valid && fc->armed) {
        if (!r->first_arm_ms) { r->first_arm_ms = now ? now : 1; }
        r->disarm_since = 0;
    } else if (fc->link_up && fc->armed_valid && !fc->armed) {
        if (!r->disarm_since) { r->disarm_since = now ? now : 1; }
    }
    bool disarm_done = r->disarm_since && (now - r->disarm_since) >= RID_WIFI_STOP_HOLD_MS &&
                       !flight_may_be_airborne();
    if (disarm_done) { r->first_arm_ms = 0; }
    bool base = en && !fault && active && rid_mode_has_wifi(c->rid.mode);
    if (!base || disarm_done) {
        r->rx_req = false;
    } else if (!r->rx_req && r->first_arm_ms && fc->link_up && fc->armed_valid && fc->armed &&
               (now - r->first_arm_ms) >= RID_WIFI_START_DELAY_MS) {
        bool raw, sw, desired; uint8_t phase, last;
        ctrl_record_status(&raw, &sw, &desired, &phase, &last);
        bool f1_settled = phase != CTRL_REC_START_DELAY && phase != CTRL_REC_VERIFY_START;
        if (f1_settled) { r->rx_req = true; }
    }
    portal_rid_rx_request(r->rx_req);
    // RID RX FAIL grace counts from the moment RID_RX is due AND the AP teardown has finished.
    uint32_t due = rid_wifi_due_ms();
    if (!r->rx_req) { if (due) { rid_set_wifi_due_ms(0); } }
    else if (!due && !portal_ap_busy()) { rid_set_wifi_due_ms(now ? now : 1); }

    uint8_t ws = rid_wifi_state();
    if (ws == RID_WIFI_RX && r->wifi_state_prev != RID_WIFI_RX) { rebaseline = true; }
    r->wifi_state_prev = ws;

    if (now - r->last_1s >= 1000u) { r->last_1s = now; rid_tick_1s(now); }
    return rebaseline;
}

static void hk_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_HOUSEKEEPING);
    uint32_t last_liveness = 0;
    // Was the aircraft armed when the previous boot ended? (RTC; false after a power cycle)
    bool armed_seen = flight_may_be_airborne();
    static rid_sup_t rid_sup;                    // Remote ID supervision state (static: small stack)

    for (;;) {
        fc_state_t fc; fc_state_get(&fc);
        // Publish the decoded arm state for every task (BLE quieting, bond-erase guard).
        // An unknown state (link down / not yet decoded) changes nothing.
        if (fc.link_up && fc.armed_valid) { flight_note_armed(fc.armed); }
        cam_link_state_t cam_link; tribool_t cam_rec;
        cam_state_get_brief(&cam_link, &cam_rec);          // not the whole ~450 B cam_state_t

        // --- flags upkeep -------------------------------------------
        if (fc.link_up) { bridge_flag_clear(FLAG_FC_OFFLINE); }
        else            { bridge_flag_set(FLAG_FC_OFFLINE); }
        // Only judge OSD support once a genuine MSP_API_VERSION reply has
        // landed (api_major > 0 -- Betaflight's real major version is always
        // >= 1). Before that, api_major/osd_supported just haven't been
        // resolved yet -- gating on it here (not a fixed boot-time budget)
        // means a flight controller that's slow to finish its own boot no
        // longer produces a false "FC too old for custom OSD" that only
        // clears once housekeeping happens to re-check.
        if (fc.link_up && fc.api_major > 0) {
            if (fc.osd_supported) { bridge_flag_clear(FLAG_OSD_UNSUPPORTED); }
            else                  { bridge_flag_set(FLAG_OSD_UNSUPPORTED); }
        }

        // --- LED -------------------------------------------------
        led_request(pick_led(&fc, cam_link, cam_rec));
        led_service();

        // --- button --------------------------------------------
        btn_event_t be = button_poll();
        if (be == BTN_SHORT) { led_identify(3000); }
        // Factory reset (spec 11.0), runtime-only gesture (>= 10 s held, same
        // pin as portal force-open) -- refused while armed, matching the
        // portal's own guard on this action (never disrupt a flying aircraft
        // for a discretionary reset). GPIO9 can't be sampled at boot for this
        // (see platform.h), so there's no boot-time equivalent any more.
        if (be == BTN_FACTORY && !fc.armed && !flight_may_be_airborne()) {   // hint outlives a lost FC link
            ESP_LOGW(TAG, "factory reset (button held 10 s)");
            cfg_factory_erase();
            cam_erase_all_bonds();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }

        // --- portal supervision (spec 10.1) -------------------
        const cfg_t *c = cfg_get();
#ifdef CONFIG_BRIDGE_BOARD_RECLINK
        bool always = true;      // official board: Wi-Fi portal always on while disarmed (off while armed)
#else
        bool always = (c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED);
#endif
        // Arm state must be KNOWN (decoded from MSP_STATUS this link session), not just
        // "link up and armed == false (the zero default)" -- spec 13.3.
        bool trig = always && fc.link_up && fc.armed_valid && !fc.armed;
        // FC link down: bench case (spec 10.1) -- but never open the AP on a
        // flying aircraft after a mid-flight reboot. Refuse if the FC has
        // reported armed at any point this boot, within the first 5 s, or on a
        // watchdog-recovery boot (spec 13.3 / 12.3).
        // armed_seen starts from the RTC hint (hk_task), so a silent-hang or brown-out
        // reboot in flight also keeps the AP off.
        if (fc.armed) { armed_seen = true; }
        if (!fc.link_up && always && !armed_seen && now_ms() > 5000 &&
            !bridge_flag_get(FLAG_WDT_RECOVERED)) {
            trig = true;
        }
        // Remote ID first: its RID_RX request and test state feed portal_supervise's AP guard.
        bool rid_rebase = rid_supervise(&rid_sup, &fc, c, now_ms());
        portal_supervise(fc.armed || flight_may_be_airborne(), trig, be == BTN_LONG);

        // --- camera liveness (spec 3.5) ----------------------
        if (now_ms() - last_liveness > 1000) {
            last_liveness = now_ms();
            cam_liveness_check();
        }

        // --- stats flush (<= 1 / 30 s) --------------------
        cfg_stats_flush();

        // --- heap low-water watch (spec 12.1) ---------------
        heap_watch(rid_rebase);

        twdt_feed(WDT_HOUSEKEEPING);
        // 10 Hz: keeps the armed-edge -> portal_stop() latency ~100 ms
        // (spec 12.4). portal_stop() only signals; portal_task tears down.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void housekeeping_start(void)
{
    static StaticTask_t tcb;
    // 4 KB: it holds an fc_state_t + cam_state_t (~580 B) and calls into logging
    // (vfprintf is deep). 2.5 KB overflowed once cam_state_t grew -- stack
    // protection fault on the first Wi-Fi client join.
    static StackType_t stack[4096 / sizeof(StackType_t)];
    xTaskCreateStatic(hk_task, "housekeeping", sizeof(stack)/sizeof(stack[0]),
                      NULL, 3, stack, &tcb);
    ESP_LOGI(TAG, "housekeeping started");
}
