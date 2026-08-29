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
