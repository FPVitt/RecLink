// portal.h -- SoftAP + captive HTTP config portal (spec 10). Bench only:
// runs only while disarmed; torn down < 100 ms on the armed edge.
#pragma once
#include <stdbool.h>

// Bring the portal up (SoftAP, HTTP server, captive DNS). Idempotent.
void portal_start(void);

// Tear everything down fast (spec 10.1). Idempotent, safe from any task.
void portal_stop(void);

bool portal_active(void);

// Confirm the running firmware is healthy so the bootloader won't roll it back
// after an OTA (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). Call once from app_main
// after init succeeds.
void portal_ota_mark_valid(void);

// housekeeping calls this at 5 Hz with the current arm state + a "trigger
// wants it open" hint; it opens/closes accordingly and enforces the timeout.
void portal_supervise(bool armed, bool trigger_open, bool button_long);

// ---- Remote ID Wi-Fi (spec 8.8.2): portal_task is the single owner of every esp_wifi_* call --
// these only REQUEST a mode. housekeeping evaluates the start / hold / stop conditions.
void portal_rid_rx_request(bool want);   // RID_RX wanted (passive receive-only, channel 6)
void portal_wifi_kick(void);             // wake portal_task (a reception-test abort, ...)
bool portal_ap_busy(void);               // an AP session is starting, up or tearing down
bool portal_rid_busy(void);              // a RID_RX / TEST session is starting, up or stopping
