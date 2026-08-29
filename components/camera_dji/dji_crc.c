#include "dji_crc.h"
#include <stdbool.h>

// Tables computed once at first use from the reflected polynomials, so there is
// no risk of a mis-transcribed 256-entry literal.
static uint16_t t16[256];
static uint32_t t32[256];
static bool built;

static void build(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c16 = i;
        for (int k = 0; k < 8; k++) { c16 = (c16 & 1) ? (c16 >> 1) ^ 0x8408 : (c16 >> 1); }
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
    uint16_t crc = 0x3692;                 // DUML seed
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc >> 8) ^ t16[(crc ^ data[i]) & 0xFF]);
    }
    return crc;
}

uint32_t dji_crc32(const uint8_t *data, size_t len)
{
    if (!built) { build(); }
    uint32_t crc = 0x00000000u;            // DUML: init 0, no final xor
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ t32[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}
