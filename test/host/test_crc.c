// CRC routines: published check values, plus the DJI DUML vector recorded in dji_crc.c.
#include "test.h"
#include "crc.h"
#include "dji_crc.h"

static void test_crc8_dvbs2(void)
{
    CHECK_EQ(crc8_dvbs2((const uint8_t *)"123456789", 9), 0xBC);    // CRC-8/DVB-S2 check value
    CHECK_EQ(crc8_dvbs2((const uint8_t *)"", 0), 0x00);
    uint8_t crc = 0;
    for (const char *s = "123456789"; *s; s++) { crc = crc8_dvbs2_update(crc, (uint8_t)*s); }
    CHECK_EQ(crc, 0xBC);                                           // streaming == one-shot
}

static void test_crc32(void)
{
    CHECK_EQ(crc32_iso_hdlc((const uint8_t *)"123456789", 9), 0xCBF43926u);   // CRC-32/ISO-HDLC check value
    CHECK_EQ(crc32_iso_hdlc((const uint8_t *)"", 0), 0x00000000u);
}

static void test_dji_crc(void)
{
    // dji-sdk/Osmo-GPS-Controller-Demo connect_cmd_frame.txt (Connection Request 0x00/0x19), header bytes [0..9]
    const uint8_t hdr[10] = { 0xAA, 0x33, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };
    CHECK_EQ(dji_crc16(hdr, sizeof hdr), 0x2F98);
    CHECK_EQ(dji_crc16(NULL, 0), 0x3AA3);          // init value, no final xor
    CHECK_EQ(dji_crc32(NULL, 0), 0x00003AA3u);
    // The two checksums must differ in width and behaviour: flipping one bit changes both.
    uint8_t b[10]; memcpy(b, hdr, 10); b[4] ^= 1;
    CHECK(dji_crc16(b, 10) != dji_crc16(hdr, 10));
    CHECK(dji_crc32(b, 10) != dji_crc32(hdr, 10));
}

int main(void)
{
    RUN(test_crc8_dvbs2);
    RUN(test_crc32);
    RUN(test_dji_crc);
    TEST_MAIN_END();
}
