// camera.h -- brand-agnostic camera layer (spec 5).
//
// One owner rule: cam_task is the ONLY task that touches the BLE link. Drivers
// run entirely in cam_task context (directly or via the NimBLE host callback,
// which cam_task serialises through cam_events). Other tasks submit work via
// cam_submit() -> cam_cmdq and read cam_state.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_uuid.h"        // ble_uuid_t for brand_gatt_t
#include "bridge_types.h"
#include "cam_state.h"

// ---- advertisement seen by the scanner -------------------------------
typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    char     name[32];
    uint8_t  mfg[31];
    uint8_t  mfg_len;
    uint16_t svc_uuid16;      // primary 16-bit service UUID if advertised (0 = none)
} ble_adv_t;

// ---- callbacks the driver may invoke (spec 5.1) ---------------------
// All of these run in cam_task context and just update cam_state / post events.
typedef struct {
    void (*on_link_state)(cam_link_state_t s);
    void (*on_state_dirty)(void);              // wake osd_task
    void (*on_ack)(cam_op_t op, bool ok);      // record/preset verify
    void (*on_paired)(const uint8_t addr[6], const char *name, uint32_t dji_id);
} camera_cbs_t;

// GATT characteristics a brand needs. cam_task owns the generic BLE sequence
// (scan -> connect -> bond -> discover -> subscribe, spec 3.3); the driver only
// declares which characteristics to subscribe and drives the protocol on top.
typedef struct {
    const ble_uuid_t *sub[4];      // value-handle chars to subscribe (NULL-term)
    uint8_t           sub_n;
} brand_gatt_t;

// ---- the driver interface (spec 5.1) --------------------------------
// Note vs spec wording: spec's `connect(addr, first_pair)` is split -- cam_task
// performs the BLE connect/bond, then calls link_up(first_pair) so the driver
// can resolve handles and run its initial protocol sync (GoPro get-all /
// DUML handshake). `disconnect` -> link_down.
typedef struct {
    const char *name;                          // "gopro" / "dji"

    bool (*adv_match)(const ble_adv_t *adv);
    const brand_gatt_t *(*gatt)(void);

    esp_err_t (*link_up)(bool first_pair);     // post-discovery protocol sync
    void      (*link_down)(void);

    esp_err_t (*record)(bool start);
    esp_err_t (*select_preset)(uint8_t slot);
    esp_err_t (*set_datetime)(int64_t utc_ms, int16_t utc_off_min, bool dst);
    esp_err_t (*get_datetime)(void);
    esp_err_t (*push_gps)(const gps_fix_t *fix, int64_t utc_ms);
    esp_err_t (*hilight)(void);

    esp_err_t (*tick_1s)(void);

    // GATT notification fan-in (cam_task routes every notify here).
    void (*on_notify)(uint16_t attr_handle, const uint8_t *data, uint16_t len);
} camera_driver_t;

// ---- registry (cam_registry.c) ------------------------------------
void camera_register(const camera_driver_t *drv);
const camera_driver_t *camera_by_name(const char *name);
const camera_driver_t *camera_match_adv(const ble_adv_t *adv);

// Drivers install their cbs pointer at register time via this getter.
const camera_cbs_t *camera_cbs(void);

// ---- cam_task control (cam_task.c) -------------------------------
void cam_start(void);        // create cam_task; call after NimBLE init

// Submit a control op (thread-safe). Policy: drop-duplicate else drop-oldest.
typedef struct { uint8_t op; int32_t arg; int64_t arg64; } cam_cmd_t;
esp_err_t cam_submit(cam_op_t op, int32_t arg);
esp_err_t cam_submit_datetime(int64_t utc_ms, int16_t utc_off_min, bool dst);
esp_err_t cam_submit_gps(const gps_fix_t *fix, int64_t utc_ms);

// Portal actions.
esp_err_t cam_pair_begin(const char *brand);   // scan-and-bond
esp_err_t cam_forget(void);                    // delete bond + cfg + go IDLE
void      cam_erase_all_bonds(void);           // factory reset path (spec 11.0)
bool      cam_reconcile_bonds(bool erase_orphan_bond);  // boot; -> FLAG_REPAIR_NEEDED

// ---- cam_events: to ctrl_task + osd_task -------------------------
typedef enum {
    CAM_EV_LINK = 0,         // value = cam_link_state_t
    CAM_EV_STATE_DIRTY,
    CAM_EV_ACK,              // value hi16 = cam_op_t, lo16 = ok
    CAM_EV_RECORDING,        // value = tribool_t
    CAM_EV_PAIRED,
} cam_ev_type_t;

typedef struct { uint8_t type; int32_t value; } cam_event_t;
QueueHandle_t cam_events(void);

// housekeeping liveness check (spec 3.5): READY but silent 30 s -> force relink.
void cam_liveness_check(void);
