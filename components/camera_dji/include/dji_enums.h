// dji_enums.h -- enum -> OSD string maps (spec 7.5 / 16.C).
// MUST be copied verbatim from dji-sdk/Osmo-GPS-Controller-Demo
// logic/enums_logic.c, pinned by commit hash in VERIFY.md (DJI-ENUMS). The
// tables below are placeholders pending that pin.
#pragma once
#include <stdint.h>

const char *dji_resolution_str(uint8_t v);   // video_resolution
const char *dji_fps_str(uint8_t v);          // fps_idx
const char *dji_mode_str(uint8_t v);         // camera_mode
