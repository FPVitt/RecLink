#include "dji_crc.h"
#include <stdbool.h>

// verified: dji-sdk/Osmo-GPS-Controller-Demo utils/crc/custom_crc16.c + custom_crc16.h
//           and utils/crc/custom_crc32.c + custom_crc32.h @ 92fe23e5a749f189593f980a26a105c3bb66aa1c (2026-08-29)
// CRC-16: reflected, table polynomial 0xA001 (= 0x8005 reflected), init 0x3AA3, no final xor.
// CRC-32: reflected, table polynomial 0xEDB88320, init 0x00003AA3 (NOT 0), no final xor.
// Test vectors (test/connect_cmd_frame_builder/connect_cmd_frame.txt, Connection Request 0x00/0x19):
//   CRC-16 over bytes[0..9]  of AA 33 00 02 00 00 00 00 01 00           -> 0x2F98
//   CRC-32 over bytes[0..46] of frame 0 (see ledger)                    -> 0x7C666C79
// Tables computed once at first use from the reflected polynomials, so there is
// no risk of a mis-transcribed 256-entry literal (verified equal to the demo's
// static crc16_table[]/crc32_table[]: table[1] == 0xC0C1 / 0x77073096).
static uint16_t t16[256];
static uint32_t t32[256];
static bool built;

static void build(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c16 = i;
        for (int k = 0; k < 8; k++) { c16 = (c16 & 1) ? (c16 >> 1) ^ 0xA001 : (c16 >> 1); }
        t16[i] = c16;

        uint32_t c32 = i;
        for (int k = 0; k < 8; k++) { c32 = (c32 & 1) ? (c32 >> 1) ^ 0xEDB88320u : (c32 >> 1); }
        t32[i] = c32;
    }
    built = true;
}

uint16_t dji_crc16(const uint8_t *data, size_t len)
{
    if (!built) { build(); }
    uint16_t crc = 0x3AA3;                 // demo crc_init()
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc >> 8) ^ t16[(crc ^ data[i]) & 0xFF]);
    }
    return crc;
}

uint32_t dji_crc32(const uint8_t *data, size_t len)
{
    if (!built) { build(); }
    uint32_t crc = 0x00003AA3u;            // demo crc32_init(); no final xor
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ t32[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}
