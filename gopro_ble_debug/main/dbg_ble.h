// dbg_ble.h -- NimBLE central for GoPro debugging: scan, connect, pair, full
// GATT discovery, and a serialized GATT worker for writes / reads / CCCDs.
//
// The bond store is the NimBLE "nimble_bond" NVS namespace shared with the
// original RecLink firmware, so a camera paired there reconnects here (and the
// other way round) without re-pairing.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "host/ble_uuid.h"

typedef enum { DBG_LINK_IDLE = 0, DBG_LINK_CONNECTING, DBG_LINK_CONNECTED } dbg_link_t;

typedef struct {
    uint8_t  addr[6];          // little-endian, as NimBLE stores it
    uint8_t  addr_type;
    int8_t   rssi;
    char     name[30];
    uint8_t  gopro;            // advertises 0xFEA6 / GoPro manufacturer data
    uint8_t  bonded;
    uint8_t  adv_len, rsp_len;
    uint8_t  adv[31], rsp[31];
    uint32_t last_ms;
} dbg_dev_t;

#define DBG_MAX_SVC 16
#define DBG_MAX_CHR 48

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t start, end;
} dbg_svc_t;

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t def_handle, val_handle, end_handle;
    uint16_t cccd;             // 0 = none found
    uint8_t  props;            // BLE_GATT_CHR_PROP_*
    uint8_t  svc;              // index into svcs[]
    uint8_t  sub;              // 0 off, 1 notify, 2 indicate (what we last wrote)
} dbg_chr_t;

typedef struct {
    int       n_svc, n_chr;
    bool      complete;
    dbg_svc_t svcs[DBG_MAX_SVC];
    dbg_chr_t chrs[DBG_MAX_CHR];
} dbg_gatt_t;

typedef struct {
    bool       synced;
    bool       scanning;
    dbg_link_t link;
    uint8_t    peer[6];
    uint8_t    peer_type;
    uint16_t   conn;
    uint16_t   mtu;
    bool       encrypted, bonded, authenticated;
    uint8_t    key_size;
    int8_t     rssi;
    bool       keepalive;
    uint32_t   keepalive_ms;
    uint32_t   tx_pkts, rx_pkts, tx_errs;
    uint32_t   connected_ms;   // uptime when the link came up
    int        last_disc_reason;
    // auto bring-up after connect: secure -> discover -> subscribe all
    bool       auto_secure, auto_discover, auto_subscribe;
} dbg_status_t;

void      dbg_ble_init(void);
void      dbg_ble_status(dbg_status_t *out);
void      dbg_ble_set_auto(bool secure, bool discover, bool subscribe);

esp_err_t dbg_ble_scan(bool on, uint32_t secs, bool active);
int       dbg_ble_devices(dbg_dev_t *out, int max);
void      dbg_ble_devices_clear(void);

esp_err_t dbg_ble_connect(const uint8_t addr[6], uint8_t addr_type);
void      dbg_ble_disconnect(void);
esp_err_t dbg_ble_secure(void);

// Queued on the GATT worker (return = queued, results arrive in evlog).
esp_err_t dbg_ble_discover(void);
esp_err_t dbg_ble_subscribe(uint16_t val_handle, uint8_t mode);   // 0/1/2
esp_err_t dbg_ble_subscribe_all(void);
// `gopro` = wrap `data` (one logical message, <= 8191 B) in Open GoPro packet
// headers and send it as 20-byte packets; else one raw ATT write.
esp_err_t dbg_ble_write(uint16_t handle, const uint8_t *data, size_t len, bool gopro, bool with_rsp);
esp_err_t dbg_ble_read(uint16_t handle);
esp_err_t dbg_ble_mtu(void);
void      dbg_ble_keepalive(bool on, uint32_t period_ms);

// Snapshot of the discovered table (copy; safe from any task).
void      dbg_ble_gatt(dbg_gatt_t *out);

// Bond store (shared with the original firmware!).
int       dbg_ble_bonds(uint8_t addrs[][6], uint8_t *types, int max);
esp_err_t dbg_ble_delete_bond(const uint8_t addr[6], uint8_t type);
esp_err_t dbg_ble_delete_all_bonds(void);
