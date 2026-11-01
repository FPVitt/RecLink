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
                                         "cam_task", "msp_task", "portal_task" };
    char sb[96]; size_t o = 0;
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
static void heap_watch(void)
{
    static uint32_t baseline_lwm;      // 0 until latched at 60 s
    static uint32_t warned_at;
    static uint32_t last_log_ms;

    uint32_t up = now_ms();
    uint32_t lwm = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

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
    if (portal_active())               { return LED_PORTAL; }
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

static void hk_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_HOUSEKEEPING);
    uint32_t last_liveness = 0;

    for (;;) {
        fc_state_t fc; fc_state_get(&fc);
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
        if (be == BTN_FACTORY && !fc.armed) {
            ESP_LOGW(TAG, "factory reset (button held 10 s)");
            cfg_factory_erase();
            cam_erase_all_bonds();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }

        // --- portal supervision (spec 10.1) -------------------
        const cfg_t *c = cfg_get();
        bool always = (c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED);
        // Arm state must be KNOWN (decoded from MSP_STATUS this link session), not just
        // "link up and armed == false (the zero default)" -- spec 13.3.
        bool trig = always && fc.link_up && fc.armed_valid && !fc.armed;
        // FC link down: bench case (spec 10.1) -- but never open the AP on a
        // flying aircraft after a mid-flight reboot. Refuse if the FC has
        // reported armed at any point this boot, within the first 5 s, or on a
        // watchdog-recovery boot (spec 13.3 / 12.3).
        static bool armed_seen;
        if (fc.armed) { armed_seen = true; }
        if (!fc.link_up && always && !armed_seen && now_ms() > 5000 &&
            !bridge_flag_get(FLAG_WDT_RECOVERED)) {
            trig = true;
        }
        portal_supervise(fc.armed, trig, be == BTN_LONG);

        // --- camera liveness (spec 3.5) ----------------------
        if (now_ms() - last_liveness > 1000) {
            last_liveness = now_ms();
            cam_liveness_check();
        }

        // --- stats flush (<= 1 / 30 s) --------------------
        cfg_stats_flush();

        // --- heap low-water watch (spec 12.1) ---------------
        heap_watch();

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
