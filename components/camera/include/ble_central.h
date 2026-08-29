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
    BLE_EVT_ENC_FAIL,
    BLE_EVT_DISC_DONE,      // GATT discovery complete
    BLE_EVT_DISC_FAIL,
} ble_central_evt_t;

typedef void (*ble_scan_cb_t)(const ble_adv_t *adv);
typedef void (*ble_evt_cb_t)(ble_central_evt_t ev, int status);
typedef void (*ble_notify_cb_t)(uint16_t val_handle, const uint8_t *data, uint16_t len);

// Start the controller + host. Idempotent. Call once from app_main.
void ble_central_init(void);
void ble_central_set_cbs(ble_scan_cb_t scan, ble_evt_cb_t evt, ble_notify_cb_t notify);

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
esp_err_t ble_central_forget_bond(const uint8_t addr[6], uint8_t addr_type);
esp_err_t ble_central_erase_all_bonds(void);
