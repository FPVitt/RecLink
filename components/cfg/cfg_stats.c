// cfg_stats.c -- deferred runtime counters, NVS namespace "stats" (spec 11.1).
// Losing the last few seconds on a crash is harmless, so these are RAM-buffered
// and flushed by housekeeping at most once per 30 s.
#include "cfg.h"
#include "monoclock.h"
#include "nvs.h"
#include "esp_log.h"

#define NS_STATS "stats"

static cfg_stats_t s_ram;
static bool        s_dirty;
static uint32_t    s_last_flush_ms;

void cfg_stats_load(cfg_stats_t *dst)
{
    nvs_handle_t h;
    if (nvs_open(NS_STATS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "reconnects", &s_ram.reconnects);
        nvs_get_u32(h, "msp_crc", &s_ram.msp_crc_errors);
        nvs_get_u32(h, "clk_fail", &s_ram.clock_sync_fail);
        nvs_close(h);
    }
    if (dst) { *dst = s_ram; }
}

void cfg_stats_bump_reconnect(void)  { s_ram.reconnects++;      s_dirty = true; }
void cfg_stats_bump_msp_crc(void)    { s_ram.msp_crc_errors++;  s_dirty = true; }

void cfg_stats_flush(void)
{
    if (!s_dirty) { return; }
    uint32_t t = now_ms();
    if (s_last_flush_ms && (t - s_last_flush_ms) < 30000) { return; }

    nvs_handle_t h;
    if (nvs_open(NS_STATS, NVS_READWRITE, &h) != ESP_OK) { return; }
    nvs_set_u32(h, "reconnects", s_ram.reconnects);
    nvs_set_u32(h, "msp_crc", s_ram.msp_crc_errors);
    nvs_set_u32(h, "clk_fail", s_ram.clock_sync_fail);
    nvs_commit(h);
    nvs_close(h);

    s_dirty = false;
    s_last_flush_ms = t;
}
