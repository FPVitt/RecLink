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
