// cam_caps.h -- what the camera says each preset-able setting may currently be
// set to (GoPro "capabilities", Open GoPro query 0x32). The options a setting
// offers depend on the camera's other settings (the fps list on the chosen
// resolution, ...), so this is a snapshot of the camera's CURRENT state, kept
// fresh by the driver. Written by cam_task (driver), read by the portal.
//
// Deliberately NOT part of cam_state_t: that struct is copied onto small task
// stacks, and this one is ~540 B.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gp_preset.h"

// The camera's own video presets (GoPro Get Available Presets), for the portal's simple
// zones: a zone loads one of these by id. The name is the preset's custom name or its
// title ("Standard", "Custom 2"); empty if the camera's title is not known to us.
#define CAM_PRESETS_MAX  16
#define CAM_PRESET_NAME  20
typedef struct { uint32_t id; char name[CAM_PRESET_NAME]; } cam_preset_t;

typedef struct {
    uint16_t gen;                       // bumps whenever the content changes
    bool     have[GP_PSET_MAX];         // camera listed options for gp_pset_id(i)
    uint8_t  bits[GP_PSET_MAX][32];     // allowed option values: bit v of bits[i]
} cam_caps_t;

void     cam_caps_init(void);
void     cam_caps_get(cam_caps_t *out);                  // any task
uint16_t cam_caps_gen(void);                             // any task, cheap
void     cam_caps_publish(const cam_caps_t *c);          // driver / cam_task only (setting options)
void     cam_caps_publish_presets(const cam_preset_t *p, int n);   // driver / cam_task only; bumps gen on a change
int      cam_caps_presets(cam_preset_t *out, int max);  // any task: copies the list, returns its length (0 = unknown)

static inline bool cam_caps_has(const cam_caps_t *c, int i, uint8_t v)
{
    return c->have[i] && (c->bits[i][v >> 3] & (1u << (v & 7)));
}
