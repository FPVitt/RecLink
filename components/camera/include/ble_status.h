// ble_status.h -- classification of NimBLE encryption-change statuses. No NimBLE include, so it can be
// unit-tested on the host (test/host); ble_central.c static-asserts that the constants below still
// match the NimBLE headers.
#pragma once
#include <stdbool.h>

#define BLESTAT_EAUTHEN          23      // BLE_HS_EAUTHEN
#define BLESTAT_HCI_BASE         0x200   // BLE_HS_ERR_HCI_BASE  (+ HCI error code)
#define BLESTAT_SM_US_BASE       0x400   // BLE_HS_ERR_SM_US_BASE  (+ SM error code)
#define BLESTAT_SM_PEER_BASE     0x500   // BLE_HS_ERR_SM_PEER_BASE (+ SM error code)
#define BLESTAT_HCI_AUTH_FAIL    0x05    // HCI: authentication failure
#define BLESTAT_HCI_PINKEY_MISS  0x06    // HCI: PIN or key missing (peer has no key for us)

// Only a key/authentication failure says the camera lost (or never had) our bond. A timeout, a
// link dropped mid-encryption or a busy controller -- the usual result at the edge of range --
// must never count towards erasing the bond (it would strand the camera until a bench re-pair).
static inline bool ble_enc_status_is_bond_loss(int st)
{
    if (st == BLESTAT_EAUTHEN) { return true; }
    if (st == BLESTAT_HCI_BASE + BLESTAT_HCI_AUTH_FAIL || st == BLESTAT_HCI_BASE + BLESTAT_HCI_PINKEY_MISS) { return true; }
    // Security Manager protocol errors, raised by us or reported by the peer (pairing refused, confirm failed ...).
    if (st >= BLESTAT_SM_US_BASE   && st < BLESTAT_SM_US_BASE   + 0x100) { return true; }
    if (st >= BLESTAT_SM_PEER_BASE && st < BLESTAT_SM_PEER_BASE + 0x100) { return true; }
    return false;
}
