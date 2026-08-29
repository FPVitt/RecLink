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
int osd_format_line(char *out, size_t out_cap, const char *tmpl,
                    const fc_state_t *fc, const cam_state_t *cam,
                    const cfg_t *cfg, bool blink_phase);

// Compute the current highest-priority warning string ("" if none).
const char *osd_active_warning(const fc_state_t *fc, const cam_state_t *cam,
                               const cfg_t *cfg);
