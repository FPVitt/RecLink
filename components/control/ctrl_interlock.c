// ctrl_interlock.c -- optional arming interlock (spec 8.6). DEFAULT DISABLED.
// Only ever BLOCKS arming while already disarmed; fails open on every doubt.
#include "control.h"
#include "msp.h"
#include "applog.h"
#include <string.h>

// Betaflight MSP_SET_ARMING_DISABLED payload: [cmd u8] 1 = block, 0 = allow.
static void send_block(bool block)
{
    static int8_t last = -1;
    if (last == (int8_t)block) { return; }
    uint8_t p[1] = { block ? 1 : 0 };
    if (msp_send(MSP_SET_ARMING_DISABLED, p, 1) == ESP_OK) {
        last = block;
        applog_note("interlock: %s arming", block ? "BLOCK" : "allow");
    }
}

void ctrl_interlock_tick(const ctrl_ctx_t *c)
{
    const cfg_t *cfg = c->cfg;
    if (!cfg->interlock.enabled) { return; }

    // Fail-open guard: only ever act when the FC link is healthy AND we are
    // certain we are disarmed (spec 8.6). Any doubt -> release.
    if (!c->fc.link_up || c->armed_now) {
        send_block(false);
        return;
    }

    bool condition_met;
    if (cfg->interlock.require == INTERLOCK_RECORDING) {
        condition_met = (c->cam.recording == TRIBOOL_TRUE);
    } else {  // INTERLOCK_CONNECTED
        condition_met = (c->cam.link == CAM_LINK_READY);
    }

    send_block(!condition_met);
}

void ctrl_interlock_failopen(void)
{
    uint8_t p[1] = { 0 };
    msp_send(MSP_SET_ARMING_DISABLED, p, 1);
}
