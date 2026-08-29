#include "gopro_enums.h"

// ---------------------------------------------------------------------------
// VERIFY (VERIFY.md: GOPRO-SETTING-2/3/121): these are the HERO 11/12 Open GoPro
// 2.0 enum values. HERO 13 adds entries. Unknown value -> "" so the OSD blanks
// the token rather than guessing (spec 9.4).
// ---------------------------------------------------------------------------

const char *gopro_resolution_str(uint8_t v)
{
    switch (v) {
    case 1:  return "4K";
    case 4:  return "2.7K";
    case 6:  return "2.7K";   // 4:3
    case 7:  return "1440";
    case 9:  return "1080";
    case 18: return "4K";     // 4:3
    case 24: return "5.3K";
    case 25: return "5K";
    case 26: return "5.3K";   // 8:7
    case 27: return "5.3K";   // 4:3
    case 28: return "4K";     // 8:7
    case 100: return "5.3K";
    case 107: return "5.3K";
    case 108: return "4K";
    case 109: return "4K";
    default: return "";
    }
}

const char *gopro_fps_str(uint8_t v)
{
    switch (v) {
    case 0:  return "240";
    case 1:  return "120";
    case 2:  return "100";
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
    case 0:   return "W";     // Wide
    case 2:   return "N";     // Narrow
    case 3:   return "SV";    // Superview
    case 4:   return "L";     // Linear
    case 7:   return "MW";    // Max SuperView
    case 8:   return "LL";    // Linear + Horizon Leveling
    case 9:   return "HV";    // HyperView
    case 10:  return "LL";
    case 11:  return "WL";
    case 12:  return "NL";
    default:  return "";
    }
}
