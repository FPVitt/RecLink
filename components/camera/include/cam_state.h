// cam_state.h -- the camera-side shared model (spec 3.4). Written ONLY by
// cam_task (incl. driver callbacks that run in cam_task context). Every
// camera-reported field carries a validity flag so the OSD can enforce
// blank-on-unconfirmed (spec 9.4).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bridge_types.h"
#include "gp_preset.h"

// Zone-preset apply progress (GoPro settings pushed by RecLink, see gp_preset.h).
typedef enum {
    GPA_NONE = 0,     // nothing requested / zone has no settings
    GPA_WAITING,      // waiting for the camera (BLE-ready, or a video preset)
    GPA_APPLYING,     // writing / verifying
    GPA_OK,           // every setting confirmed on the camera
    GPA_PARTIAL,      // some settings rejected or not confirmed
    GPA_FAILED,       // none confirmed
} gp_apply_state_t;

typedef struct {
    cam_link_state_t link;
    uint32_t last_rx_ms;
    int8_t   rssi;
    char     name[32];
    uint8_t  addr[6];

    tribool_t recording;
    tribool_t pre_record;      // camera armed but not yet capturing (DJI pre-rec); UNKNOWN elsewhere
    tribool_t busy;
    val_u8_t  battery_pct;
    val_u32_t sd_remaining_kb;
    val_u32_t rec_elapsed_s;
    val_u32_t rec_remaining_s;
    val_str_t resolution;
    val_str_t framerate;
    val_str_t aspect;          // "16:9" / "4:3" / "8:7" / "9:16"
    val_str_t lens;
    val_str_t profile;         // "STD" / "HDR" / "LOG" / "HLG"  (GoPro setting 184)
    val_u8_t  bit_depth;       // 8 or 10  (GoPro setting 183)
    // GoPro Video + Protune (unofficial settings, portal status page only).
    val_str_t iso_max;         // setting 13   (video only)
    val_str_t iso_min;         // setting 102  (video only)
    val_str_t shutter;         // setting 145  (video only)
    val_str_t protune;         // setting 114  "ON"/"OFF"
    val_str_t wb;              // setting 115
    val_str_t color;           // setting 116
    val_str_t sharpness;       // setting 117
    val_str_t ev;              // setting 118
    val_str_t tune_10bit;      // setting 174  "ON"/"OFF"
    val_u8_t  mode;             // brand enum value
    val_str_t mode_name;        // mapped string for OSD ("VID"/"PHOTO"/...)
    bool      sd_missing;       // status 33 / DJI capacity==0 -> "NO SD"
    bool      sd_full;
    bool      hot;              // GoPro status 6

    // Raw option value the camera last reported for each setting a zone can push
    // (gp_pset_id(i)). id is filled by the driver; used to verify an apply and by
    // the portal's "capture from camera".
    struct { uint8_t id; bool valid; uint8_t v; } gp_set[GP_PSET_MAX];
    char     model[24];         // GoPro model name from Get Hardware Info ("HERO11 Black"); "" until known
    uint8_t  gp_apply_zone;     // 1..3, 0 = none
    uint8_t  gp_apply_state;    // gp_apply_state_t
    uint16_t gp_apply_fail;     // bit i = gp_pset_id(i) was rejected / not confirmed

    // camera clock for F6. GoPro: real. DJI: never valid (blind sync).
    val_i64_t cam_utc_ms;
    uint32_t  cam_clock_read_uptime_ms;
} cam_state_t;

void cam_state_init(void);

// External readers (osd_task / ctrl_task / portal / housekeeping): atomic copy.
void cam_state_get(cam_state_t *dst);

// Just the link state + last-RX time, for callers that must not afford a whole
// cam_state_t on a small task stack (it is ~450 B; housekeeping has 4 KB).
void cam_state_get_link(cam_link_state_t *link, uint32_t *last_rx_ms);

// cam_task context ONLY (cam_task + the driver callbacks it invokes). Returns
// the single shared scratch; mutate in place then call cam_state_commit().
// Asserts if called from any other task.
cam_state_t *cam_scratch(void);
void         cam_state_commit(void);   // publish the scratch to readers

// Convenience for the OSD/portal: link is usable.
static inline bool cam_link_ready(const cam_state_t *s) { return s->link == CAM_LINK_READY; }
