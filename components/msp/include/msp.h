// msp.h -- MSPv2 codec + the FC UART owner task (spec 4).
//
// One owner rule (spec 1.2.3): msp_task is the ONLY code that touches the FC
// UART. Other tasks reach the FC through msp_send() (queued to msp_cmdq) and
// msp_osd_send_slot() (queued to osd_txq). fc_events carries debounced FC edges
// out to ctrl_task.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ---- MSP function codes used (spec 4.2 / 16.A) ----------------------------
enum {
    MSP_API_VERSION      = 1,
    MSP_FC_VARIANT       = 2,
    MSP_STATUS           = 101,
    MSP_RC               = 105,
    MSP_RAW_GPS          = 106,
    MSP_ANALOG           = 110,
    MSP_BOXIDS           = 119,
    MSP_BATTERY_STATE    = 130,
    MSP_OSD_CANVAS       = 189,
    MSP_RTC              = 247,
    MSP_SET_OSD_CONFIG   = 85,
    MSP_EEPROM_WRITE     = 250,
    MSP2_GET_TEXT        = 0x3006,
    MSP2_SET_TEXT        = 0x3007,
};

// ---- opcodes the bridge MUST NEVER transmit (spec 1.2.1 / 4.6 / 13.1) ------
// Enforced structurally at the transmit boundary in msp_task.c, not by
// convention. The bridge is a passenger: it never influences arming, RC, the
// FC clock, GPS, or takes over the OSD.
enum {
    MSP_SET_RAW_RC          = 200,
    MSP_SET_RAW_GPS         = 201,
    MSP_SET_RTC             = 246,
    MSP_SET_ARMING_DISABLED = 99,
    MSP_DISPLAYPORT         = 182,
    MSP_REBOOT              = 68,
};

// MSP2 text sub-ids (spec 4.4). CUSTOM_MSG slot 0..3 -> sub_id 7..10.
#define MSP2TEXT_CUSTOM_MSG_0 7
#define OSD_CUSTOM_MSG_COUNT  4
#define MSP_TEXT_MAX          16

// ---- lifecycle ----------------------------------------------------------
void msp_start(void);                   // creates msp_task; call after cfg_init

// ---- outbound (thread-safe, queued) -----------------------------------
// Enqueue an arbitrary MSP request/command. payload may be NULL.
// Policy: block up to 5 ms then drop (spec 3.3).
esp_err_t msp_send(uint16_t func, const uint8_t *payload, uint16_t len);

// Enqueue one OSD slot string (spec 4.4). slot 0..3, text truncated to 16.
// Policy: drop-oldest.
esp_err_t msp_osd_send_slot(uint8_t slot, const char *text);

// ---- fc_events: debounced edges to ctrl_task -------------------------
typedef enum {
    FC_EV_ARMED = 0,       // value: 1 = armed, 0 = disarmed  (debounced, spec 4.3)
    FC_EV_RC_CHANGED,      // value: unused (ctrl re-reads fc_state.rc)
    FC_EV_GPS_GOOD,        // value: 1/0 sustained gps_good
    FC_EV_RTC_VALID,       // value: 1 when rtc first became valid
    FC_EV_LINK,            // value: 1/0 link_up
    FC_EV_READY,           // value: 1 once initial FC handshake done
} fc_ev_type_t;

typedef struct { uint8_t type; int32_t value; } fc_event_t;

QueueHandle_t msp_fc_events(void);       // consumed by ctrl_task only

// Force-refresh OSD canvas + api on demand (called on cam reconnect etc).
void msp_request_refresh(void);
