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

typedef struct {
    uint16_t gen;                       // bumps whenever the content changes
    bool     have[GP_PSET_MAX];         // camera listed options for gp_pset_id(i)
    uint8_t  bits[GP_PSET_MAX][32];     // allowed option values: bit v of bits[i]
} cam_caps_t;

void     cam_caps_init(void);
void     cam_caps_get(cam_caps_t *out);                  // any task
uint16_t cam_caps_gen(void);                             // any task, cheap
void     cam_caps_publish(const cam_caps_t *c);          // driver / cam_task only

static inline bool cam_caps_has(const cam_caps_t *c, int i, uint8_t v)
{
    return c->have[i] && (c->bits[i][v >> 3] & (1u << (v & 7)));
}
