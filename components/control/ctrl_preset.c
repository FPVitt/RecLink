// ctrl_preset.c -- F3 preset/mode change on a radio switch (spec 8.3).
#include "control.h"
#include "camera.h"
#include "applog.h"
#include <string.h>
#include "esp_log.h"

static struct {
    int      current_band;   // -1 = unset
    int      cand_band;
    uint32_t cand_since;
    bool     reassert_pending;
} S;

void ctrl_preset_reset(void) { memset(&S, 0, sizeof(S)); S.current_band = -1; S.cand_band = -1; }

static int decode_band(const cfg_t *cfg, const fc_state_t *fc)
{
    uint8_t ch = cfg->preset.channel;
    if (ch == 0) { return -1; }
    uint8_t idx = 3 + ch;
    if (fc->rc_channel_count <= idx) { return -1; }
    uint16_t v = fc->rc[idx];
    for (int k = 0; k < 3; k++) {
        if (v <= cfg->preset.bands[k].max_us) { return k; }
    }
    return 2;   // catch-all
}

void ctrl_preset_tick(const ctrl_ctx_t *c)
{
    const cfg_t *cfg = c->cfg;
    if (!cfg->preset.enabled) { S.current_band = -1; return; }

    if (c->cam_ready_edge && S.current_band >= 0) {
        S.reassert_pending = true;   // camera may have been changed manually
    }

    int band = decode_band(cfg, &c->fc);
    if (band < 0) { return; }

    if (band != S.cand_band) { S.cand_band = band; S.cand_since = c->now_ms; }

    bool stable = (c->now_ms - S.cand_since) >= cfg->preset.dwell_ms;
    bool want_change = stable && (band != S.current_band || S.reassert_pending);
    if (!want_change) { return; }

    if (c->cam.link != CAM_LINK_READY) { return; }
    if (c->cam.busy == TRIBOOL_TRUE) { return; }   // wait (retry next tick, spec cap 3 s via dwell)

    // Do not switch preset while recording unless allowed (spec 8.3).
    if (c->cam.recording == TRIBOOL_TRUE && !cfg->preset.allow_during_record) {
        return;   // queued implicitly: we retry once recording stops
    }

    cam_submit(CAM_OP_SELECT_PRESET, band);
    applog_note("preset -> band %d (id %lu)", band, (unsigned long)cfg->preset.bands[band].id);
    S.current_band = band;
    S.reassert_pending = false;
}
