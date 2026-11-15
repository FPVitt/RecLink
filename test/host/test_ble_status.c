// Encryption-failure classification: only key/authentication failures may count towards erasing a
// bond. Timeouts, a dropped link or a busy controller (the usual edge-of-range failures) must not.
#include "test.h"
#include "ble_status.h"

static void test_bond_loss_statuses(void)
{
    CHECK(ble_enc_status_is_bond_loss(23));            // BLE_HS_EAUTHEN
    CHECK(ble_enc_status_is_bond_loss(0x200 + 0x06));  // HCI: PIN or key missing
    CHECK(ble_enc_status_is_bond_loss(0x200 + 0x05));  // HCI: authentication failure
    CHECK(ble_enc_status_is_bond_loss(0x400 + 0x04));  // SM error raised by us (confirm failed)
    CHECK(ble_enc_status_is_bond_loss(0x500 + 0x05));  // SM error reported by the peer (pairing not supported)
    CHECK(ble_enc_status_is_bond_loss(0x500 + 0xFF));
}

static void test_transient_statuses(void)
{
    CHECK(!ble_enc_status_is_bond_loss(0));            // success is not a failure at all
    CHECK(!ble_enc_status_is_bond_loss(13));           // BLE_HS_ETIMEOUT
    CHECK(!ble_enc_status_is_bond_loss(7));            // BLE_HS_ENOTCONN (link dropped mid-encryption)
    CHECK(!ble_enc_status_is_bond_loss(6));            // BLE_HS_EBUSY
    CHECK(!ble_enc_status_is_bond_loss(0x200 + 0x08)); // HCI: connection timeout (supervision)
    CHECK(!ble_enc_status_is_bond_loss(0x200 + 0x13)); // HCI: remote user terminated
    CHECK(!ble_enc_status_is_bond_loss(0x200 + 0x3E)); // HCI: connection failed to be established
    CHECK(!ble_enc_status_is_bond_loss(0x600));        // outside every SM range
    CHECK(!ble_enc_status_is_bond_loss(-1));
}

int main(void)
{
    RUN(test_bond_loss_statuses);
    RUN(test_transient_statuses);
    TEST_MAIN_END();
}
