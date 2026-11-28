// gopro_presets.h -- the camera's own preset list (Open GoPro "Get Available Presets",
// protobuf feature 0xF5 / action 0x72, reply 0xF2 NotifyPresetStatus). Pure parsing, no
// IDF dependencies (host-tested).
//
// verified: gopro/OpenGoPro @ main 0f963572611c4410a15678531e9681a6ff874edb (2026-04-15):
//   protobuf/preset_status.proto (NotifyPresetStatus / PresetGroup / Preset / EnumPresetTitle),
//   protobuf/request_get_preset_status.proto, and
//   demos/python/sdk_wireless_camera_control/open_gopro/api/ble_commands.py get_preset_status
//   (sent on the query characteristic, FeatureId.QUERY 0xF5, ActionId 0x72 / reply 0xF2).
#pragma once
#include <stdint.h>
#include "cam_caps.h"

#define GOPRO_PRESET_GROUP_VIDEO 1000u

// The VIDEO group's visible presets from a NotifyPresetStatus body (the bytes after
// [0xF5][0xF2]). Returns how many were written to out (<= max); -1 if the message does not
// parse (nothing usable). Names: custom name, else title (+ number when it has one).
int gopro_presets_parse(const uint8_t *p, uint16_t len, cam_preset_t *out, int max);

// EnumPresetTitle -> a short readable name, "" if unknown.
const char *gopro_preset_title_str(uint32_t t);
