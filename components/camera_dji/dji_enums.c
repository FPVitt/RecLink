#include "dji_enums.h"

// VERIFY.md DJI-ENUMS -- placeholder maps. Replace with the verbatim tables from
// the pinned demo commit. Unknown -> "" so the OSD blanks (spec 9.4).

const char *dji_resolution_str(uint8_t v)
{
    switch (v) {
    case 0:  return "1080";
    case 1:  return "1080";
    case 4:  return "2.7K";
    case 10: return "4K";
    case 16: return "4K";
    case 45: return "4K";     // 4K 4:3 on Action 4/5
    default: return "";
    }
}

const char *dji_fps_str(uint8_t v)
{
    switch (v) {
    case 0:  return "24";
    case 1:  return "25";
    case 2:  return "30";
    case 3:  return "48";
    case 4:  return "50";
    case 5:  return "60";
    case 6:  return "100";
    case 7:  return "120";
    case 8:  return "200";
    case 9:  return "240";
    default: return "";
    }
}

const char *dji_mode_str(uint8_t v)
{
    switch (v) {
    case 0:  return "VID";
    case 1:  return "PHOTO";
    case 2:  return "SLOMO";
    case 5:  return "TLAPSE";
    case 6:  return "HYPER";
    case 9:  return "NIGHT";
    default: return "";
    }
}
