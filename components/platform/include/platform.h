// platform.h -- board wiring, LED, button, watchdog aggregation, RTC-retained
// crash counters. The only file that knows GPIO numbers.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ---------------------------------------------------------------------------
// LED patterns (spec 10.6). housekeeping owns the LED and picks the highest
// pattern that applies; callers just report facts via led_set_*.
// ---------------------------------------------------------------------------
typedef enum {
    LED_BOOT = 0,        // solid
    LED_FC_DOWN,         // 2 short blinks / 2 s
    LED_CAM_SEARCHING,   // slow breathe
    LED_CAM_IDLE,        // 1 short blink / 3 s
    LED_RECORDING,       // solid (green)
    LED_CAM_LOST,        // fast blink
    LED_PORTAL,          // double-blink / 1 s (blue)
    LED_FAULT,           // 5 fast blinks once
    LED_IDENTIFY,        // 3 s bright blink override
    LED_PATTERN_COUNT,
} led_pattern_t;

void platform_init(void);

// LED --------------------------------------------------------------------
void led_service(void);                 // called from housekeeping @ 5 Hz+
void led_request(led_pattern_t p);      // set the "base" pattern
void led_identify(uint32_t ms);         // temporary override

// Button ----------------------------------------------------------------
typedef enum {
    BTN_NONE = 0,
    BTN_SHORT,           // < 1 s
    BTN_LONG,            // >= 3 s  (held, released)
} btn_event_t;

// Poll from housekeeping. Returns an event once per gesture.
btn_event_t button_poll(void);

// Sampled once, in platform_init, before anything else runs.
bool     button_held_at_boot_long(void);   // >= 3 s at power-up -> force portal
bool     button_held_at_boot_factory(void);// >= 10 s at power-up -> factory reset

// Task Watchdog aggregation (spec 3.5) --------------------------------
typedef enum {
    WDT_MSP = 0,
    WDT_CAM,
    WDT_CTRL,
    WDT_HOUSEKEEPING,
    WDT_TASK_COUNT,
} wdt_task_t;

void twdt_register(wdt_task_t which);   // call from each subscribed task once
void twdt_feed(wdt_task_t which);       // call once per loop

// RTC-retained counters -- survive a watchdog/brown-out reboot (spec 12.6).
typedef struct {
    uint32_t boot_count;
    uint32_t wdt_recoveries;
    uint32_t brownout_recoveries;
    uint32_t last_reset_reason;   // esp_reset_reason_t
    char     last_disconnect[24];
} crash_stats_t;

void         rtcnv_boot_update(void);          // bump counters based on reset reason
crash_stats_t rtcnv_get(void);
void         rtcnv_set_last_disconnect(const char *why);
void         rtcnv_note_wdt_recovered_ack(void);
bool         rtcnv_wdt_recovered_unacked(void);   // -> FLAG_WDT_RECOVERED
