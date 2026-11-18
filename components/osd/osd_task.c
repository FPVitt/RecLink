// osd_task.c -- builds 4 lines every refresh and pushes them to osd_txq. Sends
// ALL four slots each cycle (spec 4.4 / 9.7), including "CAM LOST" when the
// camera link is down -- never leave a slot unsent.
#include "osd.h"
#include "msp.h"
#include "monoclock.h"
#include "platform.h"
#include "rid.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "osd";

// ---- Remote ID %rp (spec 8.8.7; owner decisions 2026-11-18) --------------------------------------
// ONE aircraft per message, "!RIDx <dist> <bearing arrow><+|->" (x = the aircraft's id 1..9, only the
// "!" blinks). rid_task publishes every displayed aircraft sorted level-descending then nearest;
// rid_seq_select() picks the one on screen now: ALERTs have priority and are the ONLY messages shown
// while any exists (cycled, the whole %rp line taken over); otherwise WARN / NOTICE messages are cycled
// with the normal line in between. During its phase a message REPLACES the whole first layout line
// containing %rp (osd_compose_lines); in the normal-line phase %rp expands to nothing. Priority:
// ALERT > RID RX FAIL > WARN / NOTICE. fault_mask is built here, at render time, from the three
// single-writer words -- never stored. Inactive (no GPS gate) or rid.warn.enabled off -> nothing at
// all, not even RID RX FAIL.
static void build_rp(const fc_state_t *fc, const cfg_t *cfg, uint32_t now, bool blink_phase,
                     osd_rp_t *rp)
{
    static bool     fail_prev;
    static uint32_t fail_since;
    memset(rp, 0, sizeof(*rp));
    rp->pt.idx[0] = rp->pt.idx[1] = -1;
    if (!cfg->rid.enabled || !cfg->rid.warn.enabled || !rid_active()) { fail_prev = false; return; }

    uint8_t fm = rid_fault_mask(cfg, now);
    rid_summary_t sm;
    rid_summary_get(&sm);
    // Blank-on-unconfirmed: a summary rid_task has not refreshed (a hung task, FLAG_RID_FAULT)
    // never keeps a distance on screen.
    bool fresh = !bridge_flag_get(FLAG_RID_FAULT) &&
                 (now - sm.upd_ms) <= (uint32_t)cfg->rid.stale_s * 1000u + 1000u;
    uint8_t n = (fresh && (sm.flags & RID_SUM_HAVE)) ? (sm.n_msg > RID_MAX_AC ? RID_MAX_AC : sm.n_msg) : 0;
    rid_seq_t sq;
    rid_seq_select(sm.msg, n, now, &sq);

    bool fail = fm != 0;
    if (fail && !fail_prev) { fail_since = now; }           // each episode restarts the blink
    fail_prev = fail;

    rid_render_in_t in = { 0 };
    if (n && sq.alert)            { in.kind = RID_R_PROX; }
    else if (fail)                { in.kind = RID_R_FAIL; }
    else if (n && sq.replace)     { in.kind = RID_R_PROX; }
    else                          { return; }               // normal-line phase (or nothing to say)

    bool visible = true;
    if (in.kind == RID_R_PROX) {
        const rid_msg_t *m = &sm.msg[sq.idx];
        in.level = m->level;
        in.index = m->id;
        in.bang_on = rid_bang_on(m->level, now, cfg->osd.refresh_hz, cfg->rid.warn.blink);
        in.dist_m = m->dist_m;
        in.brg_deg = m->brg_deg;
        in.vert = m->vert;
        in.hdg_valid = fc->att_valid && fc->hdg_aligned;    // a wrong arrow is worse than none
        in.hdg_deg = fc->yaw_deg;
        in.bearing = cfg->rid.warn.bearing;
        in.imperial = cfg->osd.units == 1;
    } else {
        // RID RX FAIL: whole text blinks for the first 5 s of an episode (forced steady below 4 Hz).
        bool blink_ok = cfg->rid.warn.blink && cfg->osd.refresh_hz >= 4;
        if (blink_ok && now - fail_since < 5000u) { visible = !blink_phase; }
    }

    rid_render(rp->text, sizeof(rp->text), &in, &rp->pt);
    if (!visible) { rp->text[0] = '\0'; rp->pt.idx[0] = rp->pt.idx[1] = -1; }
    rp->replace = true;
}

static void render_all(const fc_state_t *fc, const cam_state_t *cam, const cfg_t *cfg,
                       bool blink, const osd_rp_t *rp)
{
    char line[4][20];
    osd_compose_lines(line, fc, cam, cfg, blink, rp);
    for (int i = 0; i < 4; i++) { msp_osd_send_slot((uint8_t)i, line[i]); }
}

static void osd_task(void *arg)
{
    (void)arg;
    // WDT-subscribed: a hung osd_task leaves the LAST text frozen on the goggles (REC while the
    // camera has actually stopped), which is worse than the ~300 ms reboot a stall costs.
    twdt_register(WDT_OSD);
    const cfg_t *cfg = cfg_get();
    uint8_t hz = cfg ? cfg->osd.refresh_hz : 4;
    if (hz < 1) { hz = 1; }
    if (hz > 10) { hz = 10; }
    TickType_t period = pdMS_TO_TICKS(1000 / hz);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        fc_state_t fc;   fc_state_get(&fc);
        cam_state_t cam; cam_state_get(&cam);

        uint32_t now = now_ms();
        bool blink = ((now / 500) & 1) != 0;

        if (fc.link_up && fc.osd_supported) {
            const cfg_t *c = cfg_get();                // re-fetched: commits publish a new snapshot
            osd_rp_t rp;
            build_rp(&fc, c, now, blink, &rp);
            render_all(&fc, &cam, c, blink, &rp);
        }
        twdt_feed(WDT_OSD);
        vTaskDelayUntil(&last, period);
    }
}

void osd_start(void)
{
    static StaticTask_t tcb;
    static StackType_t stack[3584 / sizeof(StackType_t)];   // holds fc+cam copies (~580 B)
    xTaskCreateStatic(osd_task, "osd_task", sizeof(stack)/sizeof(stack[0]),
                      NULL, 8, stack, &tcb);
    ESP_LOGI(TAG, "osd_task started");
}
