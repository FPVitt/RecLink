// control.h -- record/preset/hilight/clock/interlock state machine (spec 8).
// Runs in ctrl_task; consumes fc_events + cam_events, emits cam_cmdq + OSD warn
// latches + (opt-in) MSP_SET_ARMING_DISABLED.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fc_state.h"
#include "cam_state.h"
#include "cfg.h"

void ctrl_start(void);

// Shared per-tick context handed to every sub-module.
typedef struct {
    const cfg_t *cfg;
    fc_state_t   fc;
    cam_state_t  cam;
    uint32_t     now_ms;

    // edges latched from the event queues this tick
    bool arm_edge;      bool armed_now;
    bool cam_ready_edge;                 // link entered READY this tick
    bool gps_good_sustained_edge;        // gps_good has been true >= settle
    bool rtc_valid_edge;
    bool ack_seen;  uint8_t ack_op;  bool ack_ok;
} ctrl_ctx_t;

// sub-modules
void ctrl_record_reset(void);
void ctrl_record_tick(const ctrl_ctx_t *c);
// record-trigger phases, mirrored here so the portal can name them without
// duplicating the enum (ctrl_record.c owns the state machine).
enum { CTRL_REC_IDLE, CTRL_REC_START_DELAY, CTRL_REC_VERIFY_START,
       CTRL_REC_STOP_DELAY, CTRL_REC_VERIFY_STOP };
// portal diagnostics (advanced mode): what the record trigger currently sees and
// intends, independent of whether the camera has caught up. `last_cmd` is the
// tribool_t of the last START/STOP we sent (0 none / 1 stop / 2 start).
void ctrl_record_status(bool *raw_active, bool *sw_active, bool *desired,
                        uint8_t *phase, uint8_t *last_cmd);

void ctrl_preset_reset(void);
void ctrl_preset_tick(const ctrl_ctx_t *c);

void ctrl_clock_reset(void);
void ctrl_clock_tick(const ctrl_ctx_t *c);
// portal "sync now": queues a forced attempt (gate still re-evaluated in tick).
int  ctrl_clock_force(char *reason, size_t rlen);
// portal /api/clock/sync: true if the F6 gate is currently satisfied, else
// false with a human reason in `why`.
bool ctrl_clock_gate_check(char *why, size_t wl);
int32_t ctrl_clock_last_delta_ms(void);
// portal status snapshot for the Clock tab.
void ctrl_clock_status(bool *readable, int32_t *delta_ms, bool *last_ok,
                       uint32_t *last_result_uptime_ms, bool *ever_synced);

void ctrl_extras_tick(const ctrl_ctx_t *c);   // hilight switch + DJI GPS push

// switch decode helper (spec 8.2), shared by record + hilight
bool ctrl_switch_active(const fc_state_t *fc, uint8_t aux_channel,
                        uint16_t threshold_us, bool prev_active);
