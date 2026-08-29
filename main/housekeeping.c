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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hk";

static led_pattern_t pick_led(const fc_state_t *fc, const cam_state_t *cam)
{
    if (portal_active())               { return LED_PORTAL; }
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

        twdt_feed(WDT_HOUSEKEEPING);
        vTaskDelay(pdMS_TO_TICKS(200));      // 5 Hz
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
