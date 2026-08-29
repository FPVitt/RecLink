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

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hk";

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
        }
        return;
    }
    // Report each fresh 4 KB of regression, at most once every 10 s.
    if (lwm + 4096 < warned_at && up - last_log_ms > 10000) {
        last_log_ms = up;
        warned_at = lwm;
        ESP_LOGW(TAG, "heap low-water regressed: %lu B (baseline %lu) -- possible post-init alloc/leak",
                 (unsigned long)lwm, (unsigned long)baseline_lwm);
    }
}

static led_pattern_t pick_led(const fc_state_t *fc, const cam_state_t *cam)
{
    if (portal_active())               { return LED_PORTAL; }
    // Surface a watchdog-recovery reboot on the LED for the first few seconds
    // after boot; the sticky flag itself stays until acked in the portal.
    if (bridge_flag_get(FLAG_WDT_RECOVERED) && now_ms() < 8000) { return LED_FAULT; }
    if (cam->recording == TRIBOOL_TRUE) { return LED_RECORDING; }
    if (!fc->link_up)                  { return LED_FC_DOWN; }
    switch (cam->link) {
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
        cam_state_t cam; cam_state_get(&cam);

        // --- flags upkeep -------------------------------------------
        if (fc.link_up) { bridge_flag_clear(FLAG_FC_OFFLINE); }
        else            { bridge_flag_set(FLAG_FC_OFFLINE); }
        if (fc.link_up && fc.osd_supported) { bridge_flag_clear(FLAG_OSD_UNSUPPORTED); }

        // --- LED -------------------------------------------------
        led_request(pick_led(&fc, &cam));
        led_service();

        // --- button --------------------------------------------
        btn_event_t be = button_poll();
        if (be == BTN_SHORT) { led_identify(3000); }

        // --- portal supervision (spec 10.1) -------------------
        const cfg_t *c = cfg_get();
        bool trig = (c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED) && !fc.armed && fc.link_up;
        // if FC link is down we allow the portal too (bench case, spec 10.1)
        if (!fc.link_up && c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED) { trig = true; }
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
    static StackType_t stack[2560 / sizeof(StackType_t)];
    xTaskCreateStatic(hk_task, "housekeeping", sizeof(stack)/sizeof(stack[0]),
                      NULL, 3, stack, &tcb);
    ESP_LOGI(TAG, "housekeeping started");
}
