// dji_crc.h -- DUML checksums (spec 7.2).
//
// VERIFY.md DJI-CRC: the spec mandates taking these verbatim from
// dji-sdk/Osmo-GPS-Controller-Demo (utils/crc/custom_crc16.c, custom_crc32.c),
// pinned by commit hash. This implementation reproduces the well-known DUML
// parameters (CRC-16: refl poly 0x8408 / init 0x3692; CRC-32: refl poly
// 0xEDB88320 / init 0). protocol-verifier must confirm against the pinned demo.
#pragma once
#include <stddef.h>
#include <stdint.h>

uint16_t dji_crc16(const uint8_t *data, size_t len);   // header CRC, over [0..9]
uint32_t dji_crc32(const uint8_t *data, size_t len);   // frame CRC, over [0..len-1]
