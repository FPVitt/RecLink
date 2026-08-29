// msp_parse.h -- MSP response payload parsers. Called only by msp_task, which
// owns the fc_state_t scratch they mutate.
#pragma once

#include "fc_state.h"
#include "msp_codec.h"

typedef struct {
    // BOXARM bit index within flightModeFlags, resolved from MSP_BOXIDS.
    // -1 until MSP_BOXIDS has been parsed (spec 4.3 step 1).
    int      arm_bit_index;
    bool     boxids_ok;
    bool     handshake_done;   // api + variant + boxids + canvas all seen once
    uint8_t  min_sats;         // mirror of cfg.clock.min_sats for gps_good gate
} msp_parse_ctx_t;

void msp_parse_ctx_init(msp_parse_ctx_t *c, uint8_t min_sats);

// Dispatch a response frame into scratch. Returns true if it changed something
// callers care about (used to decide whether to publish immediately).
bool msp_parse_frame(const msp_frame_t *f, fc_state_t *scratch, msp_parse_ctx_t *ctx);
