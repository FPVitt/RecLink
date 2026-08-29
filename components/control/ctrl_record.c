// ctrl_record.c -- F1 (record on arm) + F2 (record on switch), spec 8.1 / 8.2.
#include "control.h"
#include "camera.h"
#include "osd.h"
#include "applog.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ctrl.rec";

enum { PH_IDLE, PH_START_DELAY, PH_VERIFY_START, PH_STOP_DELAY, PH_VERIFY_STOP };

static struct {
    bool     sw_raw, sw_active;
    uint32_t sw_since;
    bool     desired;
    bool     combi_cut;          // combi mode: switch forced a stop this clip
    bool     sw_prev;

    uint8_t  phase;
    uint32_t phase_deadline;
    uint8_t  retries_left;
    uint32_t give_up_at;         // absolute cap: arm_delay + verify_timeout*(retry+1)
    bool     gave_up;            // latched on verify exhaustion; cleared on a
                                 // desired transition or a reconnect (review H1)
    bool     desired_prev;
    bool     reconcile_pending;  // waiting for cam.recording != UNKNOWN (review H3)
    uint32_t reconcile_by;
} S;

void ctrl_record_reset(void) { memset(&S, 0, sizeof(S)); S.phase = PH_IDLE; }

static bool cam_is_recording(const ctrl_ctx_t *c) { return c->cam.recording == TRIBOOL_TRUE; }
static bool cam_ready(const ctrl_ctx_t *c) { return c->cam.link == CAM_LINK_READY; }

static void issue(bool start)
{
    cam_submit(start ? CAM_OP_RECORD_START : CAM_OP_RECORD_STOP, 0);
    applog_note("rec cmd: %s", start ? "START" : "STOP");
}

void ctrl_record_tick(const ctrl_ctx_t *c)
{
    const cfg_t *cfg = c->cfg;
    uint8_t mode = cfg->record.mode;
    if (mode == REC_OFF) { S.phase = PH_IDLE; return; }

    // ---- switch decode with debounce (spec 8.2) --------------------
    bool sw_raw = ctrl_switch_active(&c->fc, cfg->record.sw.channel,
                                     cfg->record.sw.threshold_us, S.sw_active);
    if (sw_raw != S.sw_raw) { S.sw_raw = sw_raw; S.sw_since = c->now_ms; }
    if ((c->now_ms - S.sw_since) >= cfg->record.sw.debounce_ms && sw_raw != S.sw_active) {
        S.sw_active = sw_raw;
    }
    bool sw_falling = S.sw_prev && !S.sw_active;
    bool sw_rising  = !S.sw_prev && S.sw_active;
    S.sw_prev = S.sw_active;

    // ---- desired recording (spec 8.1) --------------------------
    bool armed = c->armed_now;
    bool desired;
    switch (mode) {
    case REC_ARM:            desired = armed; break;
    case REC_SWITCH:         desired = S.sw_active; break;
    case REC_ARM_AND_SWITCH: desired = (armed || S.sw_active); break;
    default:                 desired = false; break;
    }

    // combi "cut & restart" (spec 8.1): a switch falling edge forces stop even
    // while armed; the next rising edge starts a fresh clip.
    if (mode == REC_ARM_AND_SWITCH) {
        if (sw_falling && armed) { S.combi_cut = true; }
        if (sw_rising) { S.combi_cut = false; }
        if (S.combi_cut) { desired = false; }
    }
    S.desired = desired;
    if (desired != S.desired_prev) { S.gave_up = false; }   // fresh intent, retry
    S.desired_prev = desired;

    // ---- reconcile after reconnect (spec 8.1 edge case) -----------
    // Do NOT act on cam.recording == UNKNOWN (review H3): the clip may still be
    // rolling through the BLE drop. Wait for the first real status, bounded.
    if (c->cam_ready_edge) {
        S.phase = PH_IDLE;
        S.gave_up = false;
        S.reconcile_pending = true;
        S.reconcile_by = c->now_ms + cfg->record.verify_timeout_ms + 2000;
    }
    if (S.reconcile_pending && cam_ready(c)) {
        if (c->cam.recording != TRIBOOL_UNKNOWN) {
            S.reconcile_pending = false;
            if (desired != cam_is_recording(c)) {
                ESP_LOGI(TAG, "reconcile: desired=%d cam=%d", desired, cam_is_recording(c));
                issue(desired);
                S.phase = desired ? PH_VERIFY_START : PH_VERIFY_STOP;
                S.retries_left = cfg->record.retry_count;
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
                S.give_up_at = c->now_ms + (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
            }
        } else if ((int32_t)(c->now_ms - S.reconcile_by) >= 0) {
            S.reconcile_pending = false;   // camera never reported; leave it alone
        }
    }

    if (!cam_ready(c)) {
        // camera in LOST: drop commands, keep desired; reconcile on return.
        if (S.phase == PH_START_DELAY || S.phase == PH_STOP_DELAY) { S.phase = PH_IDLE; }
        return;
    }

    switch (S.phase) {
    case PH_IDLE:
        if (desired && !S.gave_up && !cam_is_recording(c)) {
            S.phase = PH_START_DELAY;
            S.phase_deadline = c->now_ms + cfg->record.arm_delay_ms;
            // Absolute cap covers a camera stuck `busy` (review H2): armed here,
            // not after issue(), so a hold that never clears still ends in REC FAIL.
            S.give_up_at = c->now_ms + cfg->record.arm_delay_ms +
                           (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
        } else if (!desired && !S.gave_up && cam_is_recording(c)) {
            S.phase = PH_STOP_DELAY;
            S.phase_deadline = c->now_ms + cfg->record.stop_delay_ms;
            S.give_up_at = c->now_ms + cfg->record.stop_delay_ms +
                           (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
        } else if (desired == cam_is_recording(c)) {
            osd_set_rec_fail(false);      // actual state matches intent
        }
        break;

    case PH_START_DELAY:
        if (!desired) { S.phase = PH_IDLE; break; }          // bounce absorbed
        if ((int32_t)(c->now_ms - S.give_up_at) >= 0) {      // stuck busy / no progress
            applog_note("REC FAIL (start never got going)");
            osd_set_rec_fail(true); S.gave_up = true; S.phase = PH_IDLE; break;
        }
        if (c->cam.busy == TRIBOOL_TRUE) { break; }          // hold while busy
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            issue(true);
            S.retries_left = cfg->record.retry_count;
            S.phase = PH_VERIFY_START;
            S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
        }
        break;

    case PH_VERIFY_START:
        if (cam_is_recording(c)) { S.phase = PH_IDLE; osd_set_rec_fail(false); break; }
        if (!desired) { S.phase = PH_IDLE; break; }
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            if (S.retries_left > 0 && (int32_t)(c->now_ms - S.give_up_at) < 0) {
                S.retries_left--;
                issue(true);
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
            } else {
                applog_note("REC FAIL (start not confirmed)");
                osd_set_rec_fail(true);
                S.gave_up = true;      // latched -- no re-entry until desired flips
                S.phase = PH_IDLE;
            }
        }
        break;

    case PH_STOP_DELAY:
        if (desired) { S.phase = PH_IDLE; break; }
        if ((int32_t)(c->now_ms - S.give_up_at) >= 0) {
            applog_note("REC FAIL (stop never got going)");
            osd_set_rec_fail(true); S.gave_up = true; S.phase = PH_IDLE; break;
        }
        if (c->cam.busy == TRIBOOL_TRUE) { break; }
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            issue(false);
            S.retries_left = cfg->record.retry_count;
            S.phase = PH_VERIFY_STOP;
            S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
        }
        break;

    case PH_VERIFY_STOP:
        if (!cam_is_recording(c)) { S.phase = PH_IDLE; osd_set_rec_fail(false); break; }
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            if (S.retries_left > 0 && (int32_t)(c->now_ms - S.give_up_at) < 0) {
                S.retries_left--;
                issue(false);
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
            } else {
                applog_note("REC FAIL (stop not confirmed)");
                osd_set_rec_fail(true);
                S.gave_up = true;
                S.phase = PH_IDLE;
            }
        }
        break;
    }
}
