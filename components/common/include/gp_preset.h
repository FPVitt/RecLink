// gp_preset.h -- the GoPro settings a RecLink zone ("preset") can push to the
// camera. Shared by cfg (validation whitelist), the GoPro driver (apply order,
// verify) and the portal. Anything not listed here can never be written to the
// camera from a stored preset.
//
// A zone does NOT load a GoPro preset; it lists (setting id, option value)
// pairs that the driver writes to the camera's active preset, in the order
// below. The order matters: resolution first, then the settings whose valid
// options depend on it (fps, lens, ...), Protune on/off before the Protune
// sub-settings.
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
        2,    // video resolution
        108,  // video aspect ratio (HERO12+)
        3,    // frames per second
        121,  // video lens
        135,  // hypersmooth
        183,  // bit depth (HERO12+)
        184,  // profile (HERO12+)
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
