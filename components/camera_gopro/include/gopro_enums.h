// gopro_enums.h -- setting-value -> OSD string maps (spec 6.5 / 16.B).
// VERIFY every row against the Open GoPro BLE spec revision for the target
// camera (HERO 9-13 differ). Tracked in VERIFY.md.
#pragma once
#include <stdint.h>

const char *gopro_resolution_str(uint8_t v);   // setting 2
const char *gopro_aspect_str(uint8_t v);       // setting 2 -> "16:9"/"4:3"/"8:7"/"9:16"
const char *gopro_fps_str(uint8_t v);          // setting 3
const char *gopro_lens_str(uint8_t v);         // setting 121
const char *gopro_profile_str(uint8_t v);      // setting 184: STD/HDR/LOG/HLG
const char *gopro_aspect108_str(uint8_t v);    // setting 108 (real aspect-ratio setting)

// Video + Protune (unofficial settings; names/options from HERO11 gpControl dump,
// see gopro_ble_debug/docs/OPEN_GOPRO_BLE_REFERENCE.md App. D). "" = unknown value.
const char *gopro_iso_str(uint8_t v);          // settings 13 (ISO max) / 102 (ISO min)
const char *gopro_shutter_str(uint8_t v);      // setting 145
const char *gopro_wb_str(uint8_t v);           // setting 115
const char *gopro_color_str(uint8_t v);        // setting 116
const char *gopro_sharpness_str(uint8_t v);    // setting 117
const char *gopro_ev_str(uint8_t v);           // setting 118
const char *gopro_onoff_str(uint8_t v);        // settings 114 (Protune) / 174 (10 Bit)
