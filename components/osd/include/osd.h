// osd.h -- token -> 4x16 line formatter + the osd_task (spec 9).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fc_state.h"
#include "cam_state.h"
#include "cfg.h"

void osd_start(void);

// ctrl_task feeds these; osd_task folds them into %wr priority (spec 9.5).
void osd_set_rec_fail(bool on);
void osd_set_clock_fail(bool on);

// Pure formatter, unit-testable. Renders one template line into out (<=16 chars
// + NUL). `blink_phase` toggles blinking warnings. Returns strlen(out).
// Short model code for the %cm token from the model string a driver stored in cam_state.model:
// "HERO11 Black" -> GP11, "HERO11 Black Mini" -> GP11M, "Osmo Action 6" -> A6, "Osmo Action 5 Pro" -> A5P.
// Returns the length written (0 for an unknown / empty model).
size_t osd_cam_model_short(char *out, size_t cap, const char *model);

int osd_format_line(char *out, size_t out_cap, const char *tmpl,
                    const fc_state_t *fc, const cam_state_t *cam,
                    const cfg_t *cfg, bool blink_phase);

// The %rp expansion for this refresh (Remote ID, spec 8.8.7), rendered once by osd_task with
// rid_render(): text (already blank in a blink-off phase), and the glyph patches (bearing arrow,
// small up/down arrow) to apply AFTER the digital filter and the 16-char clamp (idx -1 = none).
#include "rid_core.h"
typedef struct {
    char        text[17];
    rid_patch_t pt;
    bool        replace;     // replace the whole %rp line (a message / RX FAIL is on screen now)
} osd_rp_t;
// osd_format_line with the %rp token. rp == NULL renders %rp as empty.
int osd_format_line_rp(char *out, size_t out_cap, const char *tmpl,
                       const fc_state_t *fc, const cam_state_t *cam,
                       const cfg_t *cfg, bool blink_phase, const osd_rp_t *rp);

// The first layout line containing %rp (0..3), or -1: Remote ID then draws nothing on the OSD.
int osd_rp_line(const cfg_t *cfg);

// All four slot lines for one refresh (pure; host-tested). CAM LOST layout when the camera is not
// READY (spec 9.6); then, if rp->replace and a %rp line exists, that line's whole text is the
// Remote ID message -- also during CAM LOST and with no camera. Otherwise %rp expands inline on
// the first %rp line only (other %rp lines render it empty). Glyphs are patched last.
void osd_compose_lines(char line[4][20], const fc_state_t *fc, const cam_state_t *cam,
                       const cfg_t *cfg, bool blink, const osd_rp_t *rp);

// Compute the current highest-priority warning string ("" if none).
const char *osd_active_warning(const fc_state_t *fc, const cam_state_t *cam,
                               const cfg_t *cfg);
