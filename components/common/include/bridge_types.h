// bridge_types.h -- leaf value types shared across components.
//
// This header carries NO logic and NO dependencies beyond <stdint.h>/<stdbool.h>.
// It exists so msp/, camera/, control/ and osd/ can exchange values without a
// dependency cycle. Adding a camera brand never touches this file.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tri-state and validity-tagged scalars (spec 3.4 / 9.4 blank-on-unconfirmed).
// A value the camera did not confirm MUST render blank -- never 0, "--" or a
// guess. Every camera-reported field carries one of these.
// ---------------------------------------------------------------------------
typedef enum {
    TRIBOOL_UNKNOWN = 0,
    TRIBOOL_FALSE   = 1,
    TRIBOOL_TRUE    = 2,
} tribool_t;

static inline bool tribool_is(tribool_t t, bool v) {
    return t == (v ? TRIBOOL_TRUE : TRIBOOL_FALSE);
}

typedef struct { bool valid; uint8_t  v; } val_u8_t;
typedef struct { bool valid; uint16_t v; } val_u16_t;
typedef struct { bool valid; uint32_t v; } val_u32_t;
typedef struct { bool valid; int64_t  v; } val_i64_t;
typedef struct { bool valid; char     v[16]; } val_str_t;

static inline void val_str_set(val_str_t *s, const char *src) {
    s->valid = true;
    for (int i = 0; i < (int)sizeof(s->v) - 1; i++) {
        s->v[i] = src[i];
        if (!src[i]) { return; }
    }
    s->v[sizeof(s->v) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// GPS fix snapshot (from MSP_RAW_GPS). Written only by msp_task.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  fix;          // 0 none, 2 = 2D, 3 = 3D (Betaflight reports 0/1/2 -> mapped)
    uint8_t  sats;
    int32_t  lat_1e7;
    int32_t  lon_1e7;
    int32_t  alt_m;
    uint16_t spd_cm_s;
    uint16_t course_ddeg;  // 0.1 deg
} gps_fix_t;

// ---------------------------------------------------------------------------
// Camera link lifecycle (spec 5.2). One enum shared by every driver.
// ---------------------------------------------------------------------------
typedef enum {
    CAM_LINK_IDLE = 0,
    CAM_LINK_SCAN,
    CAM_LINK_CONNECTING,
    CAM_LINK_BONDING,
    CAM_LINK_SYNCING,
    CAM_LINK_READY,
    CAM_LINK_LOST,
} cam_link_state_t;

// Control operations the driver executes (fire-and-forget, spec 5.1).
typedef enum {
    CAM_OP_RECORD_START = 0,
    CAM_OP_RECORD_STOP,
    CAM_OP_SELECT_PRESET,
    CAM_OP_SET_DATETIME,
    CAM_OP_GET_DATETIME,
    CAM_OP_PUSH_GPS,
    CAM_OP_HILIGHT,
    CAM_OP_REFRESH,        // re-pull all camera state (portal "refresh" button)
} cam_op_t;
