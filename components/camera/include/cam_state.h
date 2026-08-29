// cam_state.h -- the camera-side shared model (spec 3.4). Written ONLY by
// cam_task (incl. driver callbacks that run in cam_task context). Every
// camera-reported field carries a validity flag so the OSD can enforce
// blank-on-unconfirmed (spec 9.4).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bridge_types.h"

typedef struct {
    cam_link_state_t link;
    uint32_t last_rx_ms;
    int8_t   rssi;
    char     name[32];
    uint8_t  addr[6];

    tribool_t recording;
    tribool_t busy;
    val_u8_t  battery_pct;
    val_u32_t sd_remaining_kb;
    val_u32_t rec_elapsed_s;
    val_u32_t rec_remaining_s;
    val_str_t resolution;
    val_str_t framerate;
    val_str_t lens;
    val_u8_t  mode;             // brand enum value
    val_str_t mode_name;        // mapped string for OSD
    bool      sd_missing;       // status 33 / DJI capacity==0 -> "NO SD"
    bool      sd_full;
    bool      hot;              // GoPro status 6

    // camera clock for F6. GoPro: real. DJI: never valid (blind sync).
    val_i64_t cam_utc_ms;
    uint32_t  cam_clock_read_uptime_ms;
} cam_state_t;

void cam_state_init(void);
void cam_state_get(cam_state_t *dst);
void cam_state_publish(const cam_state_t *scratch);   // cam_task only

// Convenience for the OSD/portal: link is usable.
static inline bool cam_link_ready(const cam_state_t *s) { return s->link == CAM_LINK_READY; }
