// Record-trigger state machine (ctrl_record.c) and switch decode (ctrl_switch.c): a table of the
// scenarios the firmware promises -- start on arm, confirm, retry, give up with REC FAIL, never hold
// a STOP on `busy`, reconcile after a reconnect, DJI's slower confirmation, and the switch modes.
#include "test.h"
#include "control.h"
#include "camera.h"
#include "osd.h"
#include "applog.h"

// ---- stubs for the firmware services the state machine calls ------------------------------------
static int g_starts, g_stops;
static bool g_rec_fail;
esp_err_t cam_submit(cam_op_t op, int32_t arg)
{
    (void)arg;
    if (op == CAM_OP_RECORD_START) { g_starts++; }
    if (op == CAM_OP_RECORD_STOP)  { g_stops++; }
    return ESP_OK;
}
void osd_set_rec_fail(bool on) { g_rec_fail = on; }
void applog_note(const char *fmt, ...) { (void)fmt; }

// ---- harness ------------------------------------------------------------------------------------
static cfg_t g_cfg;
static ctrl_ctx_t g_ctx;

static void setup(int mode, const char *brand)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.record.mode = (uint8_t)mode;
    g_cfg.record.arm_delay_ms = 500;
    g_cfg.record.stop_delay_ms = 1000;
    g_cfg.record.verify_timeout_ms = 800;
    g_cfg.record.retry_count = 3;
    g_cfg.record.sw.channel = 1;                 // AUX1 -> rc[4]
    g_cfg.record.sw.threshold_us = 1700;
    g_cfg.record.sw.mode = RECSW_SWITCH;
    strcpy(g_cfg.camera.brand, brand);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.cam.link = CAM_LINK_READY;
    g_ctx.cam.recording = TRIBOOL_FALSE;
    g_ctx.cam.busy = TRIBOOL_FALSE;
    g_ctx.fc.rc_channel_count = 8;
    g_ctx.fc.rc[4] = 1000;
    g_starts = g_stops = 0; g_rec_fail = false;
    ctrl_record_reset();
}

static void tick(uint32_t t)
{
    g_ctx.now_ms = t;
    ctrl_record_tick(&g_ctx);
    g_ctx.cam_ready_edge = false;               // edges are one-tick events
}

static uint8_t phase(void) { uint8_t p; ctrl_record_status(NULL, NULL, NULL, &p, NULL); return p; }
static bool desired(void) { bool d; ctrl_record_status(NULL, NULL, &d, NULL, NULL); return d; }

// ---- scenarios ----------------------------------------------------------------------------------
static void test_arm_starts_and_stops(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true;
    tick(0);    CHECK_EQ(phase(), CTRL_REC_START_DELAY); CHECK_EQ(g_starts, 0);
    tick(400);  CHECK_EQ(g_starts, 0);                                  // still inside the arm delay
    tick(500);  CHECK_EQ(g_starts, 1); CHECK_EQ(phase(), CTRL_REC_VERIFY_START);
    g_ctx.cam.recording = TRIBOOL_TRUE;
    tick(600);  CHECK_EQ(phase(), CTRL_REC_IDLE); CHECK(!g_rec_fail);
    tick(5000); CHECK_EQ(g_starts, 1);                                  // steady state sends nothing
    g_ctx.armed_now = false;
    tick(6000); CHECK_EQ(phase(), CTRL_REC_STOP_DELAY); CHECK_EQ(g_stops, 0);
    tick(7000); CHECK_EQ(g_stops, 1); CHECK_EQ(phase(), CTRL_REC_VERIFY_STOP);
    g_ctx.cam.recording = TRIBOOL_FALSE;
    tick(7100); CHECK_EQ(phase(), CTRL_REC_IDLE); CHECK(!g_rec_fail);
    CHECK_EQ(g_starts, 1); CHECK_EQ(g_stops, 1);
}

static void test_arm_bounce_is_absorbed(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true;  tick(0);
    g_ctx.armed_now = false; tick(200);                                 // dropped inside the arm delay
    CHECK_EQ(phase(), CTRL_REC_IDLE);
    tick(2000); CHECK_EQ(g_starts, 0); CHECK_EQ(g_stops, 0);
}

static void test_retries_then_rec_fail_is_latched(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true;                                             // camera keeps answering "not recording"
    tick(0); tick(500);
    CHECK_EQ(g_starts, 1);
    tick(1300); CHECK_EQ(g_starts, 2);                                  // verify timeout -> retry 1
    tick(2100); CHECK_EQ(g_starts, 3);
    tick(2900); CHECK_EQ(g_starts, 4);                                  // retry 3 of 3
    CHECK(!g_rec_fail);
    tick(3700); CHECK(g_rec_fail);                                      // out of retries -> REC FAIL
    CHECK_EQ(phase(), CTRL_REC_IDLE);
    for (uint32_t t = 4000; t < 30000; t += 100) { tick(t); }
    CHECK_EQ(g_starts, 4);                                              // latched: no endless re-sending
    // A fresh intent (disarm, re-arm) clears the latch.
    g_ctx.armed_now = false; tick(31000);
    g_ctx.armed_now = true;  tick(32000); tick(32500);
    CHECK_EQ(g_starts, 5);
}

static void test_unconfirmed_start_is_trusted_not_failed(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.cam.recording = TRIBOOL_UNKNOWN;                              // camera never reports encoding state
    g_ctx.armed_now = true;
    tick(0); tick(500);
    CHECK_EQ(g_starts, 1);
    tick(1300);
    CHECK_EQ(phase(), CTRL_REC_IDLE); CHECK(!g_rec_fail);               // unconfirmed is not failed
    for (uint32_t t = 2000; t < 10000; t += 100) { tick(t); }
    CHECK_EQ(g_starts, 1);                                              // and nothing is re-sent
    g_ctx.armed_now = false;                                            // the STOP is still owed
    tick(11000); tick(12000);
    CHECK_EQ(g_stops, 1);
}

static void test_busy_holds_start_but_never_stop(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.cam.busy = TRIBOOL_TRUE;
    g_ctx.armed_now = true;
    tick(0); tick(500); tick(1500);
    CHECK_EQ(g_starts, 0);                                              // START waits while busy
    tick(3700);                                                         // arm_delay + verify*(retry+1)
    CHECK(g_rec_fail); CHECK_EQ(g_starts, 0);                           // ... but not forever

    setup(REC_ARM, "gopro");                                            // STOP is never held on busy
    g_ctx.armed_now = true;
    tick(0); tick(500);
    g_ctx.cam.recording = TRIBOOL_TRUE; tick(600);
    g_ctx.cam.busy = TRIBOOL_TRUE;
    g_ctx.armed_now = false;
    tick(1000); tick(2000);
    CHECK_EQ(g_stops, 1);
}

static void test_stop_failure_reports_rec_fail(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true; tick(0); tick(500);
    g_ctx.cam.recording = TRIBOOL_TRUE; tick(600);
    g_ctx.armed_now = false;
    tick(1000); tick(2000);                                             // STOP sent, camera keeps recording
    CHECK_EQ(g_stops, 1);
    tick(2800); tick(3600); tick(4400);
    CHECK_EQ(g_stops, 4);
    tick(5200);
    CHECK(g_rec_fail);
    // The camera is still rolling and the STOP is still owed: once the latch clears (the camera
    // finally agrees) nothing may be forgotten, and a later disarm cycle must send STOP again.
    int stops_before = g_stops;
    for (uint32_t t = 6000; t < 20000; t += 100) { tick(t); }
    CHECK_EQ(g_stops, stops_before);                                    // latched: no endless re-sending
}

static void test_reconnect_reconciles_only_on_real_status(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = false;
    g_ctx.cam.recording = TRIBOOL_UNKNOWN;
    g_ctx.cam_ready_edge = true;                                        // link just came back READY
    tick(0);
    tick(500);
    CHECK_EQ(g_stops, 0);                                               // UNKNOWN is never acted on
    g_ctx.cam.recording = TRIBOOL_TRUE;                                 // the clip kept rolling through the drop
    tick(600);
    CHECK_EQ(g_stops, 1);                                               // reconciled with at most one command
    CHECK_EQ(g_starts, 0);
    g_ctx.cam.recording = TRIBOOL_FALSE; tick(700);
    CHECK_EQ(g_stops, 1);
}

static void test_lost_link_drops_pending_commands(void)
{
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true;
    tick(0);
    CHECK_EQ(phase(), CTRL_REC_START_DELAY);
    g_ctx.cam.link = CAM_LINK_LOST;
    tick(200);
    CHECK_EQ(phase(), CTRL_REC_IDLE);
    tick(600); tick(5000);
    CHECK_EQ(g_starts, 0);                                              // nothing is sent into a dead link
    CHECK(desired());                                                   // but the intent is remembered
}

static void test_dji_gets_a_longer_confirmation_window(void)
{
    setup(REC_ARM, "dji");                                              // portal says 800 ms; DJI needs >= 3 s
    g_ctx.armed_now = true;
    tick(0); tick(500);
    CHECK_EQ(g_starts, 1);
    tick(1300); tick(2500); tick(3400);
    CHECK_EQ(g_starts, 1);                                              // GoPro would have re-sent by now
    tick(3500);
    CHECK_EQ(g_starts, 2);
    setup(REC_ARM, "gopro");
    g_ctx.armed_now = true; tick(0); tick(500); tick(1300);
    CHECK_EQ(g_starts, 2);                                              // control: GoPro keeps the configured 800 ms
    setup(REC_ARM, "dji");
    g_cfg.record.verify_timeout_ms = 5000;                              // a longer user setting still wins
    g_ctx.armed_now = true; tick(0); tick(500); tick(3600);
    CHECK_EQ(g_starts, 1);
}

static void test_record_off_does_nothing(void)
{
    setup(REC_OFF, "gopro");
    g_ctx.armed_now = true;
    for (uint32_t t = 0; t < 5000; t += 100) { tick(t); }
    CHECK_EQ(g_starts, 0); CHECK_EQ(g_stops, 0);
}

static void test_switch_hysteresis(void)
{
    fc_state_t fc; memset(&fc, 0, sizeof fc);
    fc.rc_channel_count = 8;
    fc.rc[4] = 1800; CHECK(ctrl_switch_active(&fc, 1, 1700, false));
    fc.rc[4] = 1700; CHECK(!ctrl_switch_active(&fc, 1, 1700, false));   // must exceed the threshold to turn on
    fc.rc[4] = 1680; CHECK(ctrl_switch_active(&fc, 1, 1700, true));     // 40 us of hysteresis to turn off
    fc.rc[4] = 1660; CHECK(!ctrl_switch_active(&fc, 1, 1700, true));
    CHECK(!ctrl_switch_active(&fc, 0, 1700, true));                     // channel 0 = unassigned
    fc.rc_channel_count = 4; fc.rc[4] = 2000;
    CHECK(!ctrl_switch_active(&fc, 1, 1700, false));                    // channel not delivered by the FC
    fc.rc_channel_count = 8; fc.rc[7] = 2000;
    CHECK(ctrl_switch_active(&fc, 4, 1700, false));                     // AUX4 -> rc[7]
}

static void test_switch_mode_records_while_on(void)
{
    setup(REC_SWITCH, "gopro");
    tick(0); CHECK(!desired());
    g_ctx.fc.rc[4] = 2000; tick(100); CHECK(desired());
    tick(700); CHECK_EQ(g_starts, 1);
    g_ctx.cam.recording = TRIBOOL_TRUE; tick(800);
    g_ctx.fc.rc[4] = 1000; tick(900); CHECK(!desired());
    tick(1900); CHECK_EQ(g_stops, 1);
}

static void test_button_mode_toggles(void)
{
    setup(REC_SWITCH, "gopro");
    g_cfg.record.sw.mode = RECSW_BUTTON;
    g_ctx.fc.rc[4] = 2000; tick(0);   CHECK(desired());                 // press: on
    g_ctx.fc.rc[4] = 1000; tick(100); CHECK(desired());                 // release does nothing
    g_ctx.fc.rc[4] = 2000; tick(200); CHECK(!desired());                // press again: off
    g_ctx.fc.rc[4] = 1000; tick(300); CHECK(!desired());
}

static void test_combi_mode_cut_and_restart(void)
{
    setup(REC_ARM_AND_SWITCH, "gopro");
    g_ctx.armed_now = true; tick(0);
    CHECK(desired());                                                   // armed -> records
    g_ctx.fc.rc[4] = 2000; tick(100); CHECK(desired());                 // switch on while armed: still recording
    g_ctx.fc.rc[4] = 1000; tick(200); CHECK(!desired());                // switch OFF while armed cuts the clip ...
    tick(300);              CHECK(!desired());                          // ... and it stays cut (still armed)
    g_ctx.fc.rc[4] = 2000; tick(400); CHECK(desired());                 // switch back ON starts a fresh clip
}

static void test_strict_and_needs_both(void)
{
    setup(REC_STRICT_AND, "gopro");
    g_ctx.armed_now = true;  tick(0);   CHECK(!desired());
    g_ctx.fc.rc[4] = 2000;   tick(100); CHECK(desired());
    g_ctx.armed_now = false; tick(200); CHECK(!desired());
}

int main(void)
{
    RUN(test_arm_starts_and_stops);
    RUN(test_arm_bounce_is_absorbed);
    RUN(test_retries_then_rec_fail_is_latched);
    RUN(test_unconfirmed_start_is_trusted_not_failed);
    RUN(test_busy_holds_start_but_never_stop);
    RUN(test_stop_failure_reports_rec_fail);
    RUN(test_reconnect_reconciles_only_on_real_status);
    RUN(test_lost_link_drops_pending_commands);
    RUN(test_dji_gets_a_longer_confirmation_window);
    RUN(test_record_off_does_nothing);
    RUN(test_switch_hysteresis);
    RUN(test_switch_mode_records_while_on);
    RUN(test_button_mode_toggles);
    RUN(test_combi_mode_cut_and_restart);
    RUN(test_strict_and_needs_both);
    TEST_MAIN_END();
}
