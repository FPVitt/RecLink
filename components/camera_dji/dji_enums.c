#include "dji_enums.h"

// verified: dji-sdk/Osmo-GPS-Controller-Demo logic/enums_logic.h + enums_logic.c
//           and docs/protocol_data_segment.md (Camera Status Push 1D02)
//           @ 92fe23e5a749f189593f980a26a105c3bb66aa1c (2026-08-29)
// Numeric keys are authoritative (camera_mode_t / video_resolution_t / fps_idx_t).
// OSD strings are abbreviated to fit the 4x16 line budget; the enum->English
// text in enums_logic.c is the source for the long forms.
// Unknown -> "" so the OSD blanks (spec 9.4).

const char *dji_resolution_str(uint8_t v)
{
    switch (v) {
    case 10:  return "1080";      // 1920x1080P
    case 16:  return "4K";        // 4K 16:9
    case 45:  return "2.7K";      // 2.7K 16:9
    case 66:  return "1080";      // 1080P 9:16
    case 67:  return "2.7K";      // 2.7K 9:16
    case 95:  return "2.7K";      // 2.7K 4:3
    case 103: return "4K";        // 4K 4:3
    case 109: return "4K";        // 4K 9:16
    // photo formats (Osmo Action: 4=L 3=M ; Osmo 360: 4=30MP 3=20MP 2=12MP)
    case 4:   return "L";
    case 3:   return "M";
    case 2:   return "S";
    default:  return "";
    }
}

const char *dji_fps_str(uint8_t v)
{
    switch (v) {
    case 1:  return "24";
    case 2:  return "25";
    case 3:  return "30";
    case 4:  return "48";
    case 5:  return "50";
    case 6:  return "60";
    case 10: return "100";
    case 7:  return "120";
    case 19: return "200";
    case 8:  return "240";
    default: return "";
    }
}

const char *dji_mode_str(uint8_t v)
{
    switch (v) {
    case 0x00: return "SLOMO";    // Slow Motion
    case 0x01: return "VID";      // Video
    case 0x02: return "TLAPSE";   // Timelapse (still)
    case 0x05: return "PHOTO";    // Photo
    case 0x0A: return "HYPER";    // Hyperlapse
    case 0x1A: return "LIVE";     // Live Streaming
    case 0x23: return "UVC";      // UVC Live Streaming
    case 0x28: return "NIGHT";    // SuperNight / low-light video
    case 0x34: return "TRACK";    // Subject Tracking
    // Osmo 360 (camera_mode > 0x34 => new-protocol 1D06 also follows)
    case 0x38: return "360VID";   // Panoramic Video
    case 0x3A: return "360HYP";   // Hyperlapse 360
    case 0x3C: return "SELFIE";   // Selfie Mode 360
    case 0x3F: return "360PIC";   // Panoramic Photo
    case 0x41: return "BOOST";    // Boost Video 360
    case 0x43: return "VORTEX";   // Vortex 360
    case 0x44: return "360NGT";   // 360 SuperNight
    case 0x4A: return "SLNGT";    // Single-Lens SuperNight 360
    default:   return "";
    }
}
