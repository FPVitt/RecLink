// osd_task.c -- builds 4 lines every refresh and pushes them to osd_txq. Sends
// ALL four slots each cycle (spec 4.4 / 9.7), including "CAM LOST" when the
// camera link is down -- never leave a slot unsent.
#include "osd.h"
#include "msp.h"
#include "monoclock.h"
#include "platform.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "osd";

static void render_all(const fc_state_t *fc, const cam_state_t *cam, const cfg_t *cfg,
                       bool blink)
{
    // Disconnect display (spec 9.6): slot0 = CAM LOST / CAM ? , others blank.
    if (cam->link != CAM_LINK_READY) {
        bool searching = (cam->link == CAM_LINK_CONNECTING || cam->link == CAM_LINK_SYNCING ||
                          cam->link == CAM_LINK_SCAN);
        const char *s0 = searching ? (blink ? "" : "CAM ?") : "CAM LOST";
        msp_osd_send_slot(0, s0);
        msp_osd_send_slot(1, "");
        msp_osd_send_slot(2, "");
        msp_osd_send_slot(3, "");
        return;
    }

    for (int i = 0; i < 4; i++) {
        char line[20];
        osd_format_line(line, sizeof(line), cfg->osd.line[i], fc, cam, cfg, blink);
        msp_osd_send_slot(i, line);
    }
}

static void osd_task(void *arg)
{
    (void)arg;
    // osd_task is not WDT-subscribed (spec 3.5 lists msp/cam/ctrl/housekeeping);
    // it is low-priority and may be merged into msp_task later.
    const cfg_t *cfg = cfg_get();
    uint8_t hz = cfg ? cfg->osd.refresh_hz : 4;
    if (hz < 1) { hz = 1; }
    if (hz > 10) { hz = 10; }
    TickType_t period = pdMS_TO_TICKS(1000 / hz);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        fc_state_t fc;   fc_state_get(&fc);
        cam_state_t cam; cam_state_get(&cam);

        bool blink = ((now_ms() / 500) & 1) != 0;

        if (fc.link_up && fc.osd_supported) {
            render_all(&fc, &cam, cfg, blink);
        }
        vTaskDelayUntil(&last, period);
    }
}

void osd_start(void)
{
    static StaticTask_t tcb;
    static StackType_t stack[3584 / sizeof(StackType_t)];   // holds fc+cam copies (~580 B)
    xTaskCreateStatic(osd_task, "osd_task", sizeof(stack)/sizeof(stack[0]),
                      NULL, 8, stack, &tcb);
    ESP_LOGI(TAG, "osd_task started");
}
