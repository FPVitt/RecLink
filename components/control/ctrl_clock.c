// ctrl_clock.c -- F6 GPS clock sync (spec 8.7). Passenger-safe: reads MSP only,
// writes the camera only, never MSP_SET_RTC.
#include "control.h"
#include "camera.h"
#include "applog.h"
#include "osd.h"
#include "monoclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ctrl.clk";

#define GPS_SETTLE_S       10
#define MIN_INTERVAL_S     60
#define TX_LATENCY_MS      150
#define VERIFY_DELAY_S     3
#define RTC_MAX_AGE_MS     120000     // spec 8.7 edge case: FC link lost

enum { PH_IDLE, PH_READ_WAIT, PH_VERIFY_WAIT };

static struct {
    uint8_t  phase;
    uint32_t last_attempt_ms;
    uint32_t phase_deadline;
    uint32_t attempt_start_ms;
    uint8_t  retries_left;
    uint32_t next_periodic_ms;
    bool     force_req;
    bool     ever_synced;
    // last result for the portal
    int32_t  last_delta_ms;
    bool     last_ok;
    uint32_t last_result_ms;
} S;

void ctrl_clock_reset(void) { memset(&S, 0, sizeof(S)); }

// ---- gate (spec 8.7 table) --------------------------------------
static bool gate_ok(const ctrl_ctx_t *c, bool for_force, char *why, size_t wl)
{
    const cfg_t *cfg = c->cfg;
    #define NO(msg) do { if (why) snprintf(why, wl, msg); return false; } while (0)

    if (!cfg->clock.enabled) NO("clock sync disabled");

    if (cfg->clock.require_gps && !c->fc.gps_present) NO("no GPS on FC");

    if (cfg->clock.require_gps) {
        bool sustained = c->fc.gps_good && c->fc.gps_good_since_ms &&
                         (c->now_ms - c->fc.gps_good_since_ms) >= GPS_SETTLE_S * 1000;
        if (!sustained) NO("waiting for GPS (3D fix + sats, 10 s)");
    }
    if (!c->fc.rtc_valid) NO("FC RTC not valid yet");

    if (c->cam.link != CAM_LINK_READY) NO("camera not connected");
    if (c->cam.recording == TRIBOOL_TRUE) NO("camera recording");
    if (c->cam.busy == TRIBOOL_TRUE) NO("camera busy");

    // condition 6: loop guard (spec: force still honours 5-6)
    if (S.last_attempt_ms && (c->now_ms - S.last_attempt_ms) < MIN_INTERVAL_S * 1000) {
        NO("too soon since last attempt (60 s)");
    }
    (void)for_force;
    #undef NO
    return true;
}

int ctrl_clock_force(char *reason, size_t rlen)
{
    // We can only sanity-check statically here; the real gate runs in tick().
    S.force_req = true;
    if (reason) { snprintf(reason, rlen, "queued"); }
    return 0;
}

// Portal /api/clock/sync 412 support: evaluate the gate now, off the ctrl_task
// tick, from a freshly sampled context. Best-effort -- the authoritative gate
// still re-runs in ctrl_clock_tick().
bool ctrl_clock_gate_check(char *why, size_t wl)
{
    // Runs on the httpd task. gate_ok() takes no locks; fc_state_get /
    // cam_state_get are leaf mutexes. The only cross-task read is S.last_attempt_ms
    // (word-aligned uint32, single-core C3 / aligned S3) -- a torn-free stale
    // read at worst yields a slightly stale 412 reason. A mutex around S would
    // let the portal block ctrl_task, which is the worse trade.
    ctrl_ctx_t c = { .cfg = cfg_get(), .now_ms = now_ms() };
    fc_state_get(&c.fc);
    cam_state_get(&c.cam);
    return gate_ok(&c, true, why, wl);
}

int32_t ctrl_clock_last_delta_ms(void) { return S.last_delta_ms; }

static bool is_dji(const ctrl_ctx_t *c) { return strcmp(c->cfg->camera.brand, "dji") == 0; }

static bool fc_utc_now(int64_t *ms) { return fc_now_utc_ms(ms, RTC_MAX_AGE_MS); }

static void do_send(const ctrl_ctx_t *c)
{
    int64_t fc_ms;
    if (!fc_utc_now(&fc_ms)) { return; }
    int64_t target = fc_ms + TX_LATENCY_MS;
    cam_submit_datetime(target, c->cfg->clock.utc_offset_min, c->cfg->clock.dst);
    applog_note("F6: pushing clock %lld (off %d)", (long long)target, c->cfg->clock.utc_offset_min);
}

static void begin_attempt(const ctrl_ctx_t *c)
{
    S.last_attempt_ms = c->now_ms;
    S.attempt_start_ms = c->now_ms;
    S.retries_left = c->cfg->clock.retry_count;

    if (is_dji(c)) {
        // blind sync: just send, no read-back (spec 8.7)
        do_send(c);
        S.ever_synced = true;
        S.last_ok = true;
        S.last_result_ms = c->now_ms;
        S.phase = PH_IDLE;
        bridge_flag_clear(FLAG_CLOCK_SYNC_FAIL);
        osd_set_clock_fail(false);
        return;
    }
    // GoPro: read first
    cam_submit(CAM_OP_GET_DATETIME, 0);
    S.phase = PH_READ_WAIT;
    S.phase_deadline = c->now_ms + 2500;
}

static void finish(const ctrl_ctx_t *c, bool ok)
{
    S.last_ok = ok;
    S.last_result_ms = c->now_ms;
    S.phase = PH_IDLE;
    if (ok) {
        S.ever_synced = true;
        bridge_flag_clear(FLAG_CLOCK_SYNC_FAIL);
        osd_set_clock_fail(false);
    } else {
        bridge_flag_set(FLAG_CLOCK_SYNC_FAIL);
        osd_set_clock_fail(true);
        applog_note("F6: clock sync gave up (delta persists)");
    }
}

static void handle_read(const ctrl_ctx_t *c)
{
    bool fresh = c->cam.cam_utc_ms.valid &&
                 (int32_t)(c->cam.cam_clock_read_uptime_ms - S.attempt_start_ms) >= 0;

    if (fresh) {
        int64_t fc_ms;
        if (!fc_utc_now(&fc_ms)) { S.phase = PH_IDLE; return; }
        int64_t fc_ref = fc_ms - (int64_t)(c->now_ms - c->cam.cam_clock_read_uptime_ms);
        int64_t delta = llabs(c->cam.cam_utc_ms.v - fc_ref);
        S.last_delta_ms = (int32_t)(delta > 0x7fffffff ? 0x7fffffff : delta);

        uint32_t thr = (uint32_t)c->cfg->clock.threshold_s * 1000;
        if (delta <= thr) {
            ESP_LOGI(TAG, "clock within %lld ms -- no write", (long long)delta);
            finish(c, true);
            return;
        }
        if (S.retries_left == 0) { finish(c, false); return; }
        S.retries_left--;
        applog_note("F6: cam clock off by %lld ms -> sync (retry budget %u)",
                    (long long)delta, S.retries_left);
        do_send(c);
        S.phase = PH_VERIFY_WAIT;
        S.phase_deadline = c->now_ms + VERIFY_DELAY_S * 1000;
        return;
    }

    if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {   // read timed out
        if (S.retries_left > 0) {
            S.retries_left--;
            cam_submit(CAM_OP_GET_DATETIME, 0);
            S.attempt_start_ms = c->now_ms;
            S.phase_deadline = c->now_ms + 2500;
        } else {
            applog_note("F6: GoPro clock read failed");
            finish(c, false);
        }
    }
}

static void handle_verify(const ctrl_ctx_t *c)
{
    if ((int32_t)(c->now_ms - S.phase_deadline) < 0) { return; }
    cam_submit(CAM_OP_GET_DATETIME, 0);
    S.attempt_start_ms = c->now_ms;
    S.phase = PH_READ_WAIT;
    S.phase_deadline = c->now_ms + 2500;
}

void ctrl_clock_tick(const ctrl_ctx_t *c)
{
    char why[64];

    // active state machine first
    if (S.phase == PH_READ_WAIT) { handle_read(c); return; }
    if (S.phase == PH_VERIFY_WAIT) { handle_verify(c); return; }

    // triggers (spec 8.7 "when the check runs")
    bool trigger = false;
    if (c->cam_ready_edge)               { trigger = true; }
    if (c->gps_good_sustained_edge)      { trigger = true; }
    if (S.force_req)                     { trigger = true; }
    if (c->now_ms >= S.next_periodic_ms && S.next_periodic_ms != 0) { trigger = true; }

    if (S.next_periodic_ms == 0 && c->cam.link == CAM_LINK_READY) {
        uint32_t per = is_dji(c) ? c->cfg->clock.resync_period_min : c->cfg->clock.check_period_min;
        S.next_periodic_ms = c->now_ms + per * 60u * 1000u;
    }

    if (!trigger) { return; }

    if (!gate_ok(c, S.force_req, why, sizeof(why))) {
        if (S.force_req) { applog_note("F6 force: gate unmet: %s", why); }
        S.force_req = false;
        return;
    }
    S.force_req = false;

    // schedule the next periodic check
    uint32_t per = is_dji(c) ? c->cfg->clock.resync_period_min : c->cfg->clock.check_period_min;
    S.next_periodic_ms = c->now_ms + per * 60u * 1000u;

    begin_attempt(c);
}

// portal status accessors
void ctrl_clock_status(bool *readable, int32_t *delta_ms, bool *last_ok,
                       uint32_t *last_age_ms, bool *ever)
{
    if (readable)    { *readable = true; }   // caller overrides for DJI
    if (delta_ms)    { *delta_ms = S.last_delta_ms; }
    if (last_ok)     { *last_ok = S.last_ok; }
    if (last_age_ms) { *last_age_ms = S.last_result_ms; }
    if (ever)        { *ever = S.ever_synced; }
}
