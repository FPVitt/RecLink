// ctrl_record.c -- F1 (record on arm) + F2 (record on switch), spec 8.1 / 8.2.
#include "control.h"
#include "camera.h"
#include "osd.h"
#include "applog.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ctrl.rec";

// Must stay in the same order as CTRL_REC_* in control.h (ctrl_record_status).
enum { PH_IDLE, PH_START_DELAY, PH_VERIFY_START, PH_STOP_DELAY, PH_VERIFY_STOP };

static struct {
    bool     raw_active;         // physical switch/button level, post-hysteresis
    bool     latch;              // button mode only: toggled by each press
    bool     sw_active;          // logical "switch says recording" (switch: = raw;
                                 // button: = latch) -- what desired/combi read
    bool     desired;
    bool     combi_cut;          // combi mode: switch forced a stop this clip
    bool     sw_prev;

    // The record loop runs on OUR last command, not on the camera's reported
    // state -- some cameras (certain DJI modes, GoPro fw that ignores our status
    // registration) never report `encoding`, and gating a STOP on a confirmation
    // that never comes leaves the camera rolling forever. cam.recording is used
    // only to CONFIRM (clear rec-fail) or to notice the user changed it ON the
    // camera. Re-issuing an already-satisfied START/STOP is harmless on both.
    tribool_t cmd;               // last record command sent (UNKNOWN before any)

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

void ctrl_record_status(bool *raw_active, bool *sw_active, bool *desired,
                        uint8_t *phase, uint8_t *last_cmd)
{
    if (raw_active) { *raw_active = S.raw_active; }
    if (sw_active)  { *sw_active  = S.sw_active; }
    if (desired)    { *desired    = S.desired; }
    if (phase)      { *phase      = S.phase; }
    if (last_cmd)   { *last_cmd   = (uint8_t)S.cmd; }
}

static bool cam_ready(const ctrl_ctx_t *c) { return c->cam.link == CAM_LINK_READY; }

static void issue(bool start)
{
    S.cmd = start ? TRIBOOL_TRUE : TRIBOOL_FALSE;
    cam_submit(start ? CAM_OP_RECORD_START : CAM_OP_RECORD_STOP, 0);
    applog_note("rec cmd: %s", start ? "START" : "STOP");
}

void ctrl_record_tick(const ctrl_ctx_t *c)
{
    const cfg_t *cfg = c->cfg;
    uint8_t mode = cfg->record.mode;
    if (mode == REC_OFF) { S.phase = PH_IDLE; return; }

    // ---- switch/button decode (spec 8.2): 40 us threshold hysteresis, no
    // debounce. "switch": the AUX level directly mirrors desired recording --
    // letting go stops it. "button": momentary -- each press (crossing the
    // threshold) TOGGLES the latch once; releasing does nothing, so a
    // non-latching / spring-loaded control can still both start and stop.
    bool raw = ctrl_switch_active(&c->fc, cfg->record.sw.channel,
                                  cfg->record.sw.threshold_us, S.raw_active);
    bool raw_rising = !S.raw_active && raw;
    S.raw_active = raw;

    if (cfg->record.sw.mode == RECSW_BUTTON) {
        if (raw_rising) { S.latch = !S.latch; }
        S.sw_active = S.latch;
    } else {
        S.sw_active = raw;
        S.latch = raw;   // keep the latch in sync so a live mode switch-over
                         // (switch -> button) starts from the current level.
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
            bool cam_rec = (c->cam.recording == TRIBOOL_TRUE);
            S.cmd = c->cam.recording;          // adopt the camera's real state as our belief
            S.reconcile_pending = false;
            if (desired != cam_rec) {
                ESP_LOGI(TAG, "reconcile: desired=%d cam=%d", desired, cam_rec);
                issue(desired);
                S.phase = desired ? PH_VERIFY_START : PH_VERIFY_STOP;
                S.retries_left = cfg->record.retry_count;
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
                S.give_up_at = c->now_ms + (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
            }
        } else if ((int32_t)(c->now_ms - S.reconcile_by) >= 0) {
            S.reconcile_pending = false;   // camera never reported; keep acting on S.cmd
        }
    }

    if (!cam_ready(c)) {
        // camera in LOST: drop commands, keep desired; reconcile on return.
        if (S.phase == PH_START_DELAY || S.phase == PH_STOP_DELAY) { S.phase = PH_IDLE; }
        return;
    }

    // S.cmd (our last command) is AUTHORITATIVE for deciding whether to send a
    // START/STOP. The camera's `recording` report is only allowed to:
    //   * seed S.cmd on first contact (we have no command history), and
    //   * adopt a manual START done ON the camera (cam says TRUE, we thought
    //     FALSE) so we don't immediately re-issue.
    // It must NEVER cancel a STOP we owe the camera: a GoPro/DJI that misreports
    // or lags "not recording" would otherwise leave the clip rolling forever
    // after the switch goes low -- the bug this guards against.
    if (S.phase == PH_IDLE && c->cam.recording != TRIBOOL_UNKNOWN) {
        if (S.cmd == TRIBOOL_UNKNOWN) {
            S.cmd = c->cam.recording;
        } else if (c->cam.recording == TRIBOOL_TRUE && S.cmd == TRIBOOL_FALSE) {
            S.cmd = TRIBOOL_TRUE;         // started on the camera itself
        }
    }

    switch (S.phase) {
    case PH_IDLE:
        // Camera agrees with intent -> healthy: clear any stale REC_FAIL and the
        // give-up latch so a slow / previously-wrong camera can self-heal.
        if (c->cam.recording != TRIBOOL_UNKNOWN &&
            (c->cam.recording == TRIBOOL_TRUE) == desired) {
            osd_set_rec_fail(false);
            S.gave_up = false;
        }
        if (S.gave_up) { break; }
        if (desired && S.cmd != TRIBOOL_TRUE) {
            S.phase = PH_START_DELAY;
            S.phase_deadline = c->now_ms + cfg->record.arm_delay_ms;
            // Absolute cap covers a camera stuck `busy` (review H2): armed here,
            // not after issue(), so a hold that never clears still ends in REC FAIL.
            S.give_up_at = c->now_ms + cfg->record.arm_delay_ms +
                           (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
        } else if (!desired && S.cmd == TRIBOOL_TRUE) {
            S.phase = PH_STOP_DELAY;
            S.phase_deadline = c->now_ms + cfg->record.stop_delay_ms;
            S.give_up_at = c->now_ms + cfg->record.stop_delay_ms +
                           (uint32_t)cfg->record.verify_timeout_ms * (cfg->record.retry_count + 1);
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
        if (c->cam.recording == TRIBOOL_TRUE) { S.phase = PH_IDLE; osd_set_rec_fail(false); break; }
        if (!desired) { S.phase = PH_IDLE; break; }
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            bool contradicted = (c->cam.recording == TRIBOOL_FALSE);
            if (contradicted && S.retries_left > 0 && (int32_t)(c->now_ms - S.give_up_at) < 0) {
                S.retries_left--;
                issue(true);
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
            } else if (contradicted) {
                applog_note("REC FAIL (start rejected by camera)");
                osd_set_rec_fail(true);
                S.gave_up = true;      // latched -- no re-entry until desired flips
                S.phase = PH_IDLE;
            } else {
                // camera never reports encoding state -- command was sent, trust
                // it. Unconfirmed is not the same as failed (spec 9.4 is an OSD
                // rule). OSD "recording" stays blank until the camera speaks.
                ESP_LOGI(TAG, "start unconfirmed (no camera status) -- assuming ok");
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
        // Do NOT hold a STOP on `busy` (unlike START): shutter-off is safe to
        // send in any state, and a camera stuck `busy` must not keep recording.
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            issue(false);
            S.retries_left = cfg->record.retry_count;
            S.phase = PH_VERIFY_STOP;
            S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
        }
        break;

    case PH_VERIFY_STOP:
        if (c->cam.recording == TRIBOOL_FALSE) { S.phase = PH_IDLE; osd_set_rec_fail(false); break; }
        if (desired) { S.phase = PH_IDLE; break; }
        if ((int32_t)(c->now_ms - S.phase_deadline) >= 0) {
            bool contradicted = (c->cam.recording == TRIBOOL_TRUE);
            if (contradicted && S.retries_left > 0 && (int32_t)(c->now_ms - S.give_up_at) < 0) {
                S.retries_left--;
                issue(false);
                S.phase_deadline = c->now_ms + cfg->record.verify_timeout_ms;
            } else if (contradicted) {
                applog_note("REC FAIL (camera still recording after stop)");
                osd_set_rec_fail(true);
                S.gave_up = true;
                S.phase = PH_IDLE;
            } else {
                ESP_LOGI(TAG, "stop unconfirmed (no camera status) -- assuming ok");
                S.phase = PH_IDLE;
            }
        }
        break;
    }
}
