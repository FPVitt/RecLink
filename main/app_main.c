// app_main.c -- init order, task creation, event wiring (spec 3.6).
#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "applog.h"
#include "monoclock.h"
#include "platform.h"
#include "cfg.h"
#include "msp.h"
#include "fc_state.h"
#include "camera.h"
#include "ble_central.h"
#include "camera_gopro.h"
#include "camera_dji.h"
#include "control.h"
#include "osd.h"
#include "portal.h"

static const char *TAG = "app";

void housekeeping_start(void);

void app_main(void)
{
    applog_init();
    ESP_LOGI(TAG, "RecLink starting");

    platform_init();                       // samples boot button, RTC counters, LED
    led_request(LED_BOOT);                 // solid = booting (spec 3.6 step 2)

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    // --- step 1: NVS + cfg + bond reconcile ---------------------------
    bool cfg_reset = false, erase_orphan = false;
    ESP_ERROR_CHECK(cfg_init(&cfg_reset, &erase_orphan));
    if (cfg_reset) { bridge_flag_set(FLAG_CFG_RESET); }
    cfg_stats_load(NULL);

    if (rtcnv_wdt_recovered_unacked()) { bridge_flag_set(FLAG_WDT_RECOVERED); led_request(LED_FAULT); }

    // --- BLE host up, then reconcile cfg vs bond store ---------------
    ble_central_init();
    ble_central_set_tx_power(cfg_get()->ble_tx_power);
    vTaskDelay(pdMS_TO_TICKS(100));         // let the host sync
    camera_register(gopro_driver());
    camera_register(dji_driver());
    if (cam_reconcile_bonds(erase_orphan)) { bridge_flag_set(FLAG_REPAIR_NEEDED); }

    // --- step 3: msp_task, wait briefly for the FC link -------------
    msp_start();
    for (int i = 0; i < 40; i++) {
        fc_state_t s; fc_state_get(&s);
        // Wait for a genuine MSP_API_VERSION reply (api_major > 0), not just
        // any frame: the scheduler asks for it first, but an FC still finishing
        // its own boot can answer a later-scheduled request (MSP_STATUS/RC)
        // before that one lands, which flips link_up true with api_major/minor
        // still at their zero default -- reading that as "API 0.0, too old for
        // custom OSD" was a false positive on the very race this waits out.
        if (s.link_up && s.api_major > 0) { ESP_LOGI(TAG, "FC link up after %d ms", i * 50); break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        // FLAG_OSD_UNSUPPORTED is judged continuously by housekeeping instead
        // (gated on a real MSP_API_VERSION reply, not a fixed boot budget) --
        // a slow-booting FC could still be short of that here, and setting it
        // prematurely produced the "FC too old" flash that only cleared once
        // housekeeping caught the real answer a few seconds later.
        fc_state_t s; fc_state_get(&s);
        if (!s.link_up) { bridge_flag_set(FLAG_FC_OFFLINE); }
    }

    // --- step 5-6: camera + control + osd + housekeeping ------------
    cam_start();
    ctrl_start();
    osd_start();
    housekeeping_start();

    // --- step 7: portal open condition -----------------------------
    // Force-open-via-button is runtime-only now (housekeeping's BTN_LONG ->
    // portal_supervise), since GPIO9 can't be sampled safely at boot -- see
    // platform.h.
    const cfg_t *c = cfg_get();
    fc_state_t fc; fc_state_get(&fc);
    bool want_portal = (c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED && !fc.armed);
    if (want_portal) { portal_start(); }

    // init got this far cleanly -> tell the bootloader this image is good, so an
    // OTA-installed build isn't rolled back on the next reboot.
    portal_ota_mark_valid();

    ESP_LOGI(TAG, "init complete (%lu ms)", (unsigned long)now_ms());
}
