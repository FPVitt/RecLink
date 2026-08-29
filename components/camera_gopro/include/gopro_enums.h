// gopro_enums.h -- setting-value -> OSD string maps (spec 6.5 / 16.B).
// VERIFY every row against the Open GoPro BLE spec revision for the target
// camera (HERO 11/12/13 differ). Tracked in VERIFY.md.
#pragma once
#include <stdint.h>

const char *gopro_resolution_str(uint8_t v);   // setting 2
const char *gopro_aspect_str(uint8_t v);       // setting 2 -> "16:9"/"4:3"/"8:7"/"9:16"
const char *gopro_fps_str(uint8_t v);          // setting 3
const char *gopro_lens_str(uint8_t v);         // setting 121
const char *gopro_profile_str(uint8_t v);      // setting 184: STD/HDR/LOG/HLG
const char *gopro_aspect108_str(uint8_t v);    // setting 108 (real aspect-ratio setting)
