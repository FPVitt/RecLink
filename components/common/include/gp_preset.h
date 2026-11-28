// gp_preset.h -- the GoPro settings a RecLink zone ("preset") can push to the
// camera. Shared by cfg (validation whitelist), the GoPro driver (apply order,
// verify) and the portal. Anything not listed here can never be written to the
// camera from a stored preset.
//
// An *advanced* zone does not load a GoPro preset (a *simple* one does, by id: Load Preset);
// it lists (setting id, option value) pairs that the driver writes to the camera's active preset, in the order
// below. The order matters: colour profile first -- on a HERO12 changing it makes the
// camera restore its own saved resolution / aspect / fps / lens / ISO / WB / colour for
// that profile (bench 2026-11-28, HERO12 fw H23.01.02.44.70), so anything written before
// it is undone. Then aspect before resolution (the aspect decides which resolutions are
// offered), the settings whose options depend on the resolution (fps, lens, ...), and
// Protune on/off before the Protune sub-settings. Only the runtime apply / poll order
// uses this index; cfg stores setting ids, so reordering does not touch saved zones.
//
// IDs and option values: Open GoPro BLE spec settings 2/3/108/121/135/183/184;
// 114/13/102/145/115/116/117/118/174 are unofficial (HERO11 gpControl dump,
// gopro_ble_debug/docs/OPEN_GOPRO_BLE_REFERENCE.md App. D) and may be rejected
// by some models -- the camera's answer is reported, not assumed.
#pragma once
#include <stdint.h>

#define GP_PSET_MAX 16

// Apply order == index. Keep GP_PSET_MAX in step.
static inline uint8_t gp_pset_id(int idx)
{
    static const uint8_t k_ids[GP_PSET_MAX] = {
        184,  // profile (HERO12+) -- first, see above
        183,  // bit depth (HERO12+)
        108,  // video aspect ratio (HERO12+)
        2,    // video resolution
        3,    // frames per second
        121,  // video lens
        135,  // hypersmooth
        114,  // Protune on/off
        13,   // ISO max
        102,  // ISO min
        145,  // shutter
        115,  // white balance
        116,  // colour
        117,  // sharpness
        118,  // EV comp
        174,  // 10-bit
    };
    return (idx >= 0 && idx < GP_PSET_MAX) ? k_ids[idx] : 0;
}

// Index of a setting id in the apply order, or -1 if it is not allowed.
static inline int gp_pset_index(uint8_t id)
{
    for (int i = 0; i < GP_PSET_MAX; i++) {
        if (gp_pset_id(i) == id) { return i; }
    }
    return -1;
}
