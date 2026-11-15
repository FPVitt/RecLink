// ctrl_switch.c -- radio-switch decode (spec 8.2), shared by record + hilight. Pure logic (no
// ESP-IDF calls) so it is covered by the host tests in test/host.
#include "control.h"

// ---- switch decode (spec 8.2): 40 us hysteresis -----------------
bool ctrl_switch_active(const fc_state_t *fc, uint8_t aux_channel,
                        uint16_t threshold_us, bool prev)
{
    if (aux_channel == 0) { return false; }
    uint8_t idx = 3 + aux_channel;                  // AUX1 -> rc[4]
    if (fc->rc_channel_count <= idx) { return false; }
    uint16_t v = fc->rc[idx];
    if (prev) { return v > (threshold_us - 40); }
    return v > threshold_us;
}
