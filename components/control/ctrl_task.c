// ctrl_task.c -- 20 Hz + event driven. Snapshots fc_state/cam_state, latches
// edges from fc_events/cam_events, then runs each sub-module (spec 3.3 / 8).
#include "control.h"
#include "msp.h"
#include "camera.h"
#include "monoclock.h"
#include "platform.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "ctrl";
#define TICK_MS 50
#define CLOCK_GPS_SETTLE_S 10

static void ctrl_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_CTRL);

    QueueHandle_t fq = msp_fc_events();
    QueueHandle_t cq = cam_events();

    ctrl_record_reset();
    ctrl_preset_reset();
    ctrl_clock_reset();

    bool armed = false;
    cam_link_state_t last_link = CAM_LINK_IDLE;
    bool gps_sustained_prev = false;

    TickType_t last = xTaskGetTickCount();

    for (;;) {
        ctrl_ctx_t c;
        memset(&c, 0, sizeof(c));
        c.cfg = cfg_get();
        c.now_ms = now_ms();
        fc_state_get(&c.fc);
        cam_state_get(&c.cam);

        // ---- drain fc_events -------------------------------------
        fc_event_t fe;
        while (xQueueReceive(fq, &fe, 0) == pdTRUE) {
            switch (fe.type) {
            case FC_EV_ARMED:
                if ((bool)fe.value != armed) {
                    armed = fe.value;
                    c.arm_edge = true;
                    c.armed_now = armed;
                }
                break;
            case FC_EV_RTC_VALID:
                c.rtc_valid_edge = true;
                break;
            default: break;
            }
        }
        c.armed_now = armed;

        // ---- drain cam_events ----------------------------------
        cam_event_t ce;
        while (xQueueReceive(cq, &ce, 0) == pdTRUE) {
            if (ce.type == CAM_EV_ACK) {
                c.ack_seen = true;
                c.ack_op = (uint8_t)(ce.value >> 16);
                c.ack_ok = (ce.value & 1);
            }
        }

        // ---- derived edges ------------------------------------
        if (c.cam.link == CAM_LINK_READY && last_link != CAM_LINK_READY) {
            c.cam_ready_edge = true;
        }
        last_link = c.cam.link;

        bool gps_sustained = c.fc.gps_good && c.fc.gps_good_since_ms &&
                             (c.now_ms - c.fc.gps_good_since_ms) >= CLOCK_GPS_SETTLE_S * 1000;
        if (gps_sustained && !gps_sustained_prev) { c.gps_good_sustained_edge = true; }
        gps_sustained_prev = gps_sustained;

        // ---- run sub-modules --------------------------------
        ctrl_record_tick(&c);
        ctrl_preset_tick(&c);
        ctrl_clock_tick(&c);
        ctrl_extras_tick(&c);

        twdt_feed(WDT_CTRL);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
    }
}

// ---- extras: hilight-on-switch + DJI GPS position push (spec 1.1 / 11b) -----
void ctrl_extras_tick(const ctrl_ctx_t *c)
{
    static bool     hl_prev;
    static uint32_t next_gps_push_ms;

    // hilight / marker on a rising switch edge
    if (c->cfg->hilight.channel) {
        bool a = ctrl_switch_active(&c->fc, c->cfg->hilight.channel, 1700, hl_prev);
        if (a && !hl_prev && c->cam.link == CAM_LINK_READY) {
            cam_submit(CAM_OP_HILIGHT, 0);
        }
        hl_prev = a;
    }

    // DJI GPS position push (only DJI; GoPro driver returns NOT_SUPPORTED).
    // The push's date/time fields set the Osmo clock, so a stale/invalid FC RTC
    // must NOT be sent -- that would stamp footage 1970-01-01 (review H11).
    if (c->cfg->gps_push.enabled && c->cam.link == CAM_LINK_READY &&
        c->fc.gps_good && strcmp(c->cfg->camera.brand, "dji") == 0) {
        uint32_t period = 1000 / (c->cfg->gps_push.rate_hz ? c->cfg->gps_push.rate_hz : 1);
        if ((int32_t)(c->now_ms - next_gps_push_ms) >= 0) {
            next_gps_push_ms = c->now_ms + period;
            int64_t fc_ms = 0;
            if (fc_now_utc_ms(&fc_ms, 120000)) {
                cam_submit_gps(&c->fc.gps, fc_ms);
            }
        }
    }
}

void ctrl_start(void)
{
    static StaticTask_t tcb;
    static StackType_t stack[4096 / sizeof(StackType_t)];   // ctrl_ctx_t is ~610 B
    xTaskCreateStatic(ctrl_task, "ctrl_task", sizeof(stack)/sizeof(stack[0]),
                      NULL, 9, stack, &tcb);
    ESP_LOGI(TAG, "ctrl_task started");
}

// ---- switch decode (spec 8.2): 40 us hysteresis -----------------
bool ctrl_switch_active(const fc_state_t *fc, uint8_t aux_channel,
                        uint16_t threshold_us, bool prev)
{
    if (aux_channel == 0) { return false; }
    uint8_t idx = 3 + aux_channel;                  // AUX1 -> rc[4]
    if (fc->rc_channel_count <= idx) { return false; }
    uint16_t v = fc->rc[idx];
    if (prev) { return v > (threshold_us - 40); }
    return v > threshold_us;
}
