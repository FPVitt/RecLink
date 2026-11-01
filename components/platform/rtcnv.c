// rtcnv.c -- counters retained in RTC slow memory across a warm reboot
// (watchdog panic, brown-out, sw reset). Lost only on a true power cycle, which
// is fine -- the point is to survive the fault-driven reboots the FMEA expects.
#include "platform.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"

// RTC_NOINIT: not cleared on deep sleep wake or sw reset; garbage after power-on
// is caught by the magic check.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR crash_stats_t s_stats;
static RTC_NOINIT_ATTR bool s_wdt_unacked;

#define RTCNV_MAGIC 0x52544356u  // "RTCV"

void rtcnv_boot_update(void)
{
    esp_reset_reason_t rr = esp_reset_reason();

    if (s_magic != RTCNV_MAGIC) {
        memset(&s_stats, 0, sizeof(s_stats));
        s_wdt_unacked = false;
        s_magic = RTCNV_MAGIC;
    }

    s_stats.boot_count++;
    s_stats.last_reset_reason = (uint32_t)rr;

    switch (rr) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
    case ESP_RST_PANIC:       // a stack overflow / assert / fault is a recovery boot too (fault LED, no portal)
        s_stats.wdt_recoveries++;
        s_wdt_unacked = true;
        break;
    case ESP_RST_BROWNOUT:
        s_stats.brownout_recoveries++;
        break;
    default:
        break;
    }
}

crash_stats_t rtcnv_get(void) { return s_stats; }

void rtcnv_set_last_disconnect(const char *why)
{
    strlcpy(s_stats.last_disconnect, why ? why : "", sizeof(s_stats.last_disconnect));
}

void rtcnv_note_wdt_recovered_ack(void) { s_wdt_unacked = false; }

bool rtcnv_wdt_recovered_unacked(void) { return s_wdt_unacked; }
