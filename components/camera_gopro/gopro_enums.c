#include "gopro_enums.h"

// verified: Open GoPro BLE spec 2.0 -- gopro/OpenGoPro
//   demos/python/sdk_wireless_camera_control/open_gopro/models/constants/settings.py
//   classes VideoResolution / FramesPerSecond / VideoLens
//   @ main 0f963572611c4410a15678531e9681a6ff874edb (2026-04-15),
//   cross-checked against gh-pages 616bfb80 (2026-06-08).
// Numeric keys are authoritative; OSD strings are abbreviated for the 4x16 line
// budget (spec 16.C). Unknown value -> "" so the OSD blanks (spec 9.4).
// Coverage: HERO 9-13 Black / HERO11 Mini / MAX 2. The *_V2 (>=107) resolution
// codes are the HERO13-era set; older bodies use the <=28 codes.

const char *gopro_resolution_str(uint8_t v)
{
    switch (v) {
    case 1:   return "4K";     // NUM_4K
    case 4:   return "2.7K";   // NUM_2_7K
    case 6:   return "2.7K";   // NUM_2_7K_4_3
    case 7:   return "1440";   // NUM_1440
    case 9:   return "1080";   // NUM_1080
    case 12:  return "720";    // NUM_720
    case 18:  return "4K";     // NUM_4K_4_3
    case 21:  return "5.6K";   // NUM_5_6K
    case 24:  return "5K";     // NUM_5K
    case 25:  return "5K";     // NUM_5K_4_3
    case 26:  return "5.3K";   // NUM_5_3K_8_7
    case 27:  return "5.3K";   // NUM_5_3K_4_3
    case 28:  return "4K";     // NUM_4K_8_7
    case 31:  return "8K";     // NUM_8K
    case 35:  return "5.3K";   // NUM_5_3K_21_9
    case 36:  return "4K";     // NUM_4K_21_9
    case 37:  return "4K";     // NUM_4K_1_1
    case 38:  return "900";    // NUM_900
    case 39:  return "4K";     // NUM_4K_SPH
    case 100: return "5.3K";   // NUM_5_3K
    case 107: return "5.3K";   // NUM_5_3K_8_7_V2
    case 108: return "4K";     // NUM_4K_8_7_V2
    case 109: return "4K";     // NUM_4K_9_16_V2
    case 110: return "1080";   // NUM_1080_9_16_V2
    case 111: return "2.7K";   // NUM_2_7K_4_3_V2
    case 112: return "4K";     // NUM_4K_4_3_V2
    case 113: return "5.3K";   // NUM_5_3K_4_3_V2
    default:  return "";
    }
}

// Aspect ratio implied by the resolution code (from the enum names above).
const char *gopro_aspect_str(uint8_t v)
{
    switch (v) {
    case 6: case 18: case 25: case 27: case 111: case 112: case 113:
        return "4:3";
    case 26: case 28: case 107: case 108:
        return "8:7";
    case 109: case 110:
        return "9:16";
    case 35: case 36:
        return "21:9";
    case 37:
        return "1:1";
    case 1: case 4: case 7: case 9: case 12: case 21: case 24: case 31:
    case 38: case 100:
        return "16:9";
    default:
        return "";
    }
}

const char *gopro_fps_str(uint8_t v)
{
    switch (v) {
    case 0:  return "240";
    case 1:  return "120";
    case 2:  return "100";
    case 3:  return "90";
    case 5:  return "60";
    case 6:  return "50";
    case 8:  return "30";
    case 9:  return "25";
    case 10: return "24";
    case 13: return "200";
    case 15: return "400";
    case 16: return "360";
    case 17: return "300";
    default: return "";
    }
}

const char *gopro_lens_str(uint8_t v)
{
    switch (v) {
    case 0:   return "W";      // WIDE
    case 2:   return "N";      // NARROW
    case 3:   return "SV";     // SUPERVIEW
    case 4:   return "L";      // LINEAR
    case 7:   return "MSV";    // MAX_SUPERVIEW
    case 8:   return "LL";     // LINEAR_HORIZON_LEVELING
    case 9:   return "HV";     // HYPERVIEW
    case 10:  return "LK";     // LINEAR_HORIZON_LOCK
    case 11:  return "MHV";    // MAX_HYPERVIEW
    case 12:  return "USV";    // ULTRA_SUPERVIEW
    case 13:  return "UW";     // ULTRA_WIDE
    case 14:  return "UL";     // ULTRA_LINEAR
    case 104: return "UHV";    // ULTRA_HYPERVIEW
    default:  return "";
    }
}

// setting 184 PROFILES (verified @ OpenGoPro settings.py 0f963572):
// 0 STANDARD / 1 HDR / 2 LOG / 101 HLG_HDR.
const char *gopro_profile_str(uint8_t v)
{
    switch (v) {
    case 0:   return "STD";
    case 1:   return "HDR";
    case 2:   return "LOG";
    case 101: return "HLG";
    default:  return "";
    }
}

// setting 108 VIDEO_ASPECT_RATIO: 0 4:3 / 1 16:9 / 3 8:7 / 4 9:16 / 5 21:9 / 6 1:1.
const char *gopro_aspect108_str(uint8_t v)
{
    switch (v) {
    case 0:  return "4:3";
    case 1:  return "16:9";
    case 3:  return "8:7";
    case 4:  return "9:16";
    case 5:  return "21:9";
    case 6:  return "1:1";
    default: return "";
    }
}

// ---- Video + Protune (unofficial; HERO11 Black gpControl dump) -------------
// ISO option IDs are NOT ordered by value. Same table for min (102) and max (13).
const char *gopro_iso_str(uint8_t v)
{
    switch (v) {
    case 0: return "6400";
    case 1: return "1600";
    case 2: return "400";
    case 3: return "3200";
    case 4: return "800";
    case 7: return "200";
    case 8: return "100";
    case 9: return "Auto";
    default: return "";
    }
}

// setting 145: whole fractions of the frame rate (shutter angle); the allowed
// set depends on setting 3.
const char *gopro_shutter_str(uint8_t v)
{
    switch (v) {
    case 0:  return "Auto";
    case 3:  return "1/24";
    case 4:  return "1/25";
    case 5:  return "1/30";
    case 6:  return "1/48";
    case 7:  return "1/50";
    case 8:  return "1/60";
    case 11: return "1/96";
    case 12: return "1/100";
    case 13: return "1/120";
    case 16: return "1/192";
    case 17: return "1/200";
    case 18: return "1/240";
    case 21: return "1/400";
    case 22: return "1/480";
    case 23: return "1/960";
    case 24: return "1/1920";
    case 25: return "1/384";
    case 28: return "1/800";
    case 29: return "1/1600";
    case 30: return "1/3200";
    case 31: return "1/3840";
    default: return "";
    }
}

const char *gopro_wb_str(uint8_t v)
{
    switch (v) {
    case 0:  return "Auto";
    case 2:  return "5500K";
    case 3:  return "6500K";
    case 4:  return "Native";
    case 5:  return "4000K";
    case 7:  return "6000K";
    case 8:  return "2300K";
    case 9:  return "2800K";
    case 10: return "3200K";
    case 11: return "4500K";
    case 12: return "5000K";
    default: return "";
    }
}

const char *gopro_color_str(uint8_t v)
{
    switch (v) {
    case 1:   return "Flat";
    case 2:   return "Natural";
    case 100: return "Vibrant";
    default:  return "";
    }
}

const char *gopro_sharpness_str(uint8_t v)
{
    switch (v) {
    case 0:  return "High";
    case 1:  return "Medium";
    case 2:  return "Low";
    default: return "";
    }
}

const char *gopro_ev_str(uint8_t v)
{
    switch (v) {
    case 0:  return "+2.0";
    case 1:  return "+1.5";
    case 2:  return "+1.0";
    case 3:  return "+0.5";
    case 4:  return "0.0";
    case 5:  return "-0.5";
    case 6:  return "-1.0";
    case 7:  return "-1.5";
    case 8:  return "-2.0";
    default: return "";
    }
}

const char *gopro_onoff_str(uint8_t v)
{
    switch (v) {
    case 0:  return "OFF";
    case 1:  return "ON";
    default: return "";
    }
}
