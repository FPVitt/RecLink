#include "gopro_presets.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// ---- minimal protobuf reader (wire types 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32) ----
typedef struct { const uint8_t *p; uint16_t n, i; bool bad; } pb_t;

static uint64_t pb_varint(pb_t *b)
{
    uint64_t v = 0;
    for (int sh = 0; sh < 64; sh += 7) {
        if (b->i >= b->n) { b->bad = true; return 0; }
        uint8_t c = b->p[b->i++];
        v |= (uint64_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) { return v; }
    }
    b->bad = true;
    return 0;
}

// Next field: returns false at the end (or on a malformed message). For wire type 2 the payload
// is returned in *sub; for a varint, its value in *v.
static bool pb_next(pb_t *b, uint32_t *field, uint64_t *v, pb_t *sub)
{
    if (b->bad || b->i >= b->n) { return false; }
    uint64_t key = pb_varint(b);
    if (b->bad) { return false; }
    *field = (uint32_t)(key >> 3);
    sub->p = b->p; sub->n = 0; sub->i = 0; sub->bad = false;      // empty unless length-delimited
    switch (key & 7) {
    case 0: *v = pb_varint(b); break;
    case 1: if (b->n - b->i < 8) { b->bad = true; } else { b->i += 8; } *v = 0; break;
    case 5: if (b->n - b->i < 4) { b->bad = true; } else { b->i += 4; } *v = 0; break;
    case 2: {
        uint64_t l = pb_varint(b);
        if (b->bad || l > (uint64_t)(b->n - b->i)) { b->bad = true; return false; }
        sub->p = b->p + b->i; sub->n = (uint16_t)l; sub->i = 0; sub->bad = false;
        b->i += (uint16_t)l;
        *v = l;
        break;
    }
    default: b->bad = true; return false;
    }
    return !b->bad;
}

static bool parse_preset(pb_t *pr, cam_preset_t *out)
{
    uint32_t f; uint64_t v; pb_t s;
    bool have_id = false, visible = true;
    uint32_t id = 0, title = 0xFFFFFFFFu, num = 0;
    char custom[CAM_PRESET_NAME] = "";
    while (pb_next(pr, &f, &v, &s)) {
        switch (f) {
        case 1: id = (uint32_t)v; have_id = true; break;          // int32 (negative = 10-byte varint)
        case 3: title = (uint32_t)v; break;
        case 4: num = (uint32_t)v; break;
        case 10: {
            size_t l = s.n < CAM_PRESET_NAME - 1 ? s.n : CAM_PRESET_NAME - 1;
            for (size_t k = 0; k < l; k++) {   // printable ASCII only: it reaches JSON and the portal's HTML
                uint8_t c = s.p[k];
                custom[k] = (c >= 0x20 && c < 0x7F && !strchr("\"\\<>&", c)) ? (char)c : '?';
            }
            custom[l] = '\0';
            break;
        }
        case 11: visible = v != 0; break;
        default: break;
        }
    }
    if (pr->bad || !have_id || !visible) { return false; }
    out->id = id;
    if (custom[0]) {
        memcpy(out->name, custom, sizeof(out->name));
    } else {
        const char *t = title == 0xFFFFFFFFu ? "" : gopro_preset_title_str(title);
        if (*t && num) { snprintf(out->name, sizeof(out->name), "%s %lu", t, (unsigned long)num); }
        else           { snprintf(out->name, sizeof(out->name), "%s", t); }
    }
    return true;
}

int gopro_presets_parse(const uint8_t *p, uint16_t len, cam_preset_t *out, int max)
{
    pb_t b = { p, len, 0, false }, g, pr;
    uint32_t f; uint64_t v;
    int n = 0;
    bool any_group = false;
    while (pb_next(&b, &f, &v, &g)) {
        if (f != 1) { continue; }                                   // preset_group_array
        any_group = true;
        // Group id may follow the presets in the encoding: find it first.
        pb_t scan = g; uint32_t gf; uint64_t gv; pb_t tmp;
        uint32_t gid = 0;
        while (pb_next(&scan, &gf, &gv, &tmp)) { if (gf == 1) { gid = (uint32_t)gv; } }
        if (scan.bad) { return -1; }
        if (gid != GOPRO_PRESET_GROUP_VIDEO) { continue; }
        while (pb_next(&g, &gf, &gv, &pr)) {
            if (gf != 2 || n >= max) { continue; }                  // preset_array
            if (parse_preset(&pr, &out[n])) { n++; }
        }
        if (g.bad) { return -1; }
    }
    if (b.bad || !any_group) { return -1; }
    return n;
}

const char *gopro_preset_title_str(uint32_t t)
{
    switch (t) {
    case 0:   return "Activity";
    case 1:   return "Standard";
    case 2:   return "Cinematic";
    case 3:   return "Photo";
    case 4:   return "Live Burst";
    case 5:   return "Burst";
    case 6:   return "Night";
    case 7:   return "TimeWarp";
    case 8:   return "Time Lapse";
    case 9:   return "Night Lapse";
    case 10:  return "Video";
    case 11:  return "Slo-Mo";
    case 13:  return "Photo";
    case 14:  return "Panorama";
    case 16:  return "TimeWarp";
    case 18:  return "Custom";
    case 19:  return "Air";
    case 20:  return "Bike";
    case 21:  return "Epic";
    case 22:  return "Indoor";
    case 23:  return "Motor";
    case 24:  return "Mounted";
    case 25:  return "Outdoor";
    case 26:  return "POV";
    case 27:  return "Selfie";
    case 28:  return "Skate";
    case 29:  return "Snow";
    case 30:  return "Trail";
    case 31:  return "Travel";
    case 32:  return "Water";
    case 33:  return "Looping";
    case 34:  return "Stars";
    case 35:  return "Action";
    case 36:  return "Follow Cam";
    case 37:  return "Surf";
    case 38:  return "City";
    case 39:  return "Shaky";
    case 40:  return "Chesty";
    case 41:  return "Helmet";
    case 42:  return "Bite";
    case 43:  return "Custom Cinematic";
    case 44:  return "Vlog";
    case 45:  return "FPV";
    case 46:  return "HDR";
    case 47:  return "Landscape";
    case 48:  return "Log";
    case 49:  return "Custom Slo-Mo";
    case 50:  return "Tripod";
    case 58:  return "Basic";
    case 59:  return "Ultra Slo-Mo";
    case 60:  return "Standard Endurance";
    case 61:  return "Activity Endurance";
    case 62:  return "Cinematic Endurance";
    case 63:  return "Slo-Mo Endurance";
    case 64:  return "Stationary 1";
    case 65:  return "Stationary 2";
    case 66:  return "Stationary 3";
    case 67:  return "Stationary 4";
    case 68:  return "Video";
    case 69:  return "TimeWarp";
    case 70:  return "SuperPhoto";
    case 71:  return "Night Photo";
    case 72:  return "Video Endurance";
    case 73:  return "Highest Quality";
    case 74:  return "Extended Battery";
    case 75:  return "Longest Battery";
    case 76:  return "Star Trail";
    case 77:  return "Light Painting";
    case 78:  return "Light Trail";
    case 79:  return "Full Frame";
    case 82:  return "Standard Quality";
    case 83:  return "Basic Quality";
    case 93:  return "Highest Quality";
    case 94:  return "Custom";
    case 99:  return "Standard";
    case 100: return "HDR";
    case 106: return "Burst Slo-Mo";
    case 125: return "4:3 Video";
    case 126: return "16:9 Video";
    case 127: return "16:9 Slo-Mo";
    case 131: return "Time Lapse Video";
    case 132: return "Time Lapse Photo";
    case 133: return "Night Lapse Video";
    case 134: return "Night Lapse Photo";
    default:  return "";
    }
}
