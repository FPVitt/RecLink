// gopro_enums.h -- setting-value -> OSD string maps (spec 6.5 / 16.B).
// VERIFY every row against the Open GoPro BLE spec revision for the target
// camera (HERO 11/12/13 differ). Tracked in VERIFY.md.
#pragma once
#include <stdint.h>

const char *gopro_resolution_str(uint8_t v);   // setting 2
const char *gopro_fps_str(uint8_t v);          // setting 3
const char *gopro_lens_str(uint8_t v);         // setting 121
