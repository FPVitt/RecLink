// ble_central.h -- thin NimBLE central wrapper. One connection, one bond.
// Every callback here runs on the NimBLE host task; handlers must only update
// shared state under a mutex or post to a queue (spec 5.1 / 3.4).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "host/ble_uuid.h"
#include "camera.h"

typedef enum {
    BLE_EVT_CONNECTED = 0,
    BLE_EVT_DISCONNECTED,
    BLE_EVT_MTU_DONE,
    BLE_EVT_ENC_DONE,       // bonding/encryption established
    BLE_EVT_ENC_FAIL,       // key/authentication failure: evidence the camera dropped its side of the bond
    BLE_EVT_ENC_TRANSIENT,  // encryption failed for any other reason (timeout, link lost, busy): says nothing about the bond
    BLE_EVT_DISC_DONE,      // GATT discovery complete
    BLE_EVT_DISC_FAIL,
} ble_central_evt_t;

typedef void (*ble_scan_cb_t)(const ble_adv_t *adv);
typedef void (*ble_evt_cb_t)(ble_central_evt_t ev, int status);
typedef void (*ble_notify_cb_t)(uint16_t val_handle, const uint8_t *data, uint16_t len);
// Runs first on every advertising report, on the raw AD bytes (NimBLE host task). Returns true
// to consume the report (it is then not forwarded to the scan callback). Must be O(1),
// allocation-free and log-free (spec 8.8.3).
typedef bool (*ble_raw_adv_hook_t)(const uint8_t addr[6], int8_t rssi, const uint8_t *data, uint8_t len);

// Scan parameters (units of 0.625 ms), always with filter_duplicates = 0.
typedef struct { uint16_t itvl, window; uint8_t passive; } ble_scan_params_t;
extern const ble_scan_params_t BLE_SCAN_AGGRESSIVE;   // 60 / 50 ms active (pairing / hunt)
extern const ble_scan_params_t BLE_SCAN_GENTLE;       // 200 / 22.5 ms passive (portal AP up)

// Start the controller + host. Idempotent. Call once from app_main.
void ble_central_init(void);
void ble_central_set_tx_power(uint8_t preset);   // RF_PWR_* (cfg.h); reboot to change
// While the aircraft may be in the air the radio is capped at RF_PWR_LOW (the camera sits next to the
// board, and a 2.4 GHz RC receiver sits next to both); the configured level returns when it is not.
void ble_central_set_flight_power(bool airborne);

// True once the NimBLE host has synced (address ready, stack usable). Bond-store
// queries before this return garbage -- callers must wait (review H15).
bool ble_central_synced(void);
void ble_central_set_cbs(ble_scan_cb_t scan, ble_evt_cb_t evt, ble_notify_cb_t notify);
void ble_central_set_raw_adv_hook(ble_raw_adv_hook_t hook);
// Forward (non-consumed) adverts to the scan callback? cam_task sets this only while the camera
// is in SCAN (s_hunt_active, spec 8.8.3). Default on.
void ble_central_forward_adverts(bool on);

esp_err_t ble_central_scan(bool enable);
esp_err_t ble_central_connect(const uint8_t addr[6], uint8_t addr_type);
esp_err_t ble_central_secure(void);         // initiate pairing/encryption
esp_err_t ble_central_discover_all(void);    // svc + char + cccd

// Resolve a characteristic value handle by UUID (after BLE_EVT_DISC_DONE).
uint16_t  ble_central_val_handle(const ble_uuid_t *chr_uuid);

esp_err_t ble_central_write(uint16_t val_handle, const void *data, uint16_t len, bool with_rsp);
esp_err_t ble_central_subscribe(uint16_t val_handle, bool enable);

void   ble_central_disconnect(void);
bool   ble_central_connected(void);
int8_t ble_central_last_rssi(void);
// Gentle mode: low-duty passive scan (Wi-Fi-friendly). Applies to the next
// ble_central_scan(true) / restarts a running scan with the new params.
void      ble_central_scan_gentle(bool on);
void      ble_central_scan_params(const ble_scan_params_t *p);   // general form of the above
bool      ble_central_scan_running(void);
uint16_t  ble_central_mtu(void);              // negotiated ATT MTU (0 if not connected)
esp_err_t ble_central_forget_bond(const uint8_t addr[6], uint8_t addr_type);
esp_err_t ble_central_forget_bond_any(const uint8_t addr[6]);  // try public + random
esp_err_t ble_central_erase_all_bonds(void);
int       ble_central_bond_count(void);     // stored peer security records
