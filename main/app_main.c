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
    ESP_LOGI(TAG, "FC-Cam Bridge starting");

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
    if (button_held_at_boot_factory()) {
        ESP_LOGW(TAG, "factory reset (button held 10 s)");
        cfg_factory_erase();
        ble_central_init();
        ble_central_erase_all_bonds();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    // --- BLE host up, then reconcile cfg vs bond store ---------------
    ble_central_init();
    vTaskDelay(pdMS_TO_TICKS(100));         // let the host sync
    camera_register(gopro_driver());
    camera_register(dji_driver());
    if (cam_reconcile_bonds(erase_orphan)) { bridge_flag_set(FLAG_REPAIR_NEEDED); }

    // --- step 3: msp_task, wait briefly for the FC link -------------
    msp_start();
    for (int i = 0; i < 40; i++) {
        fc_state_t s; fc_state_get(&s);
        if (s.link_up) { ESP_LOGI(TAG, "FC link up after %d ms", i * 50); break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        fc_state_t s; fc_state_get(&s);
        if (!s.osd_supported && s.link_up) { bridge_flag_set(FLAG_OSD_UNSUPPORTED); }
        if (!s.link_up) { bridge_flag_set(FLAG_FC_OFFLINE); }
    }

    // --- step 5-6: camera + control + osd + housekeeping ------------
    cam_start();
    ctrl_start();
    osd_start();
    housekeeping_start();

    // --- step 7: portal open condition -----------------------------
    const cfg_t *c = cfg_get();
    fc_state_t fc; fc_state_get(&fc);
    bool want_portal = button_held_at_boot_long() ||
                       (c->wifi.portal_trigger == PORTAL_TRIG_ALWAYS_DISARMED && !fc.armed);
    if (want_portal && !fc.armed) { portal_start(); }

    ESP_LOGI(TAG, "init complete (%lu ms)", (unsigned long)now_ms());
}
