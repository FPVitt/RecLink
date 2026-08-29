// crc.h -- hand-rolled CRC routines (spec 3.1). No tables pulled from vendors
// except the DJI DUML CRCs, which live in components/camera_dji/ verbatim from
// the MIT demo.
#pragma once

#include <stddef.h>
#include <stdint.h>

// CRC-8/DVB-S2 (poly 0xD5, init 0x00) -- MSPv2 frame checksum (spec 4.1).
uint8_t crc8_dvbs2_update(uint8_t crc, uint8_t b);
uint8_t crc8_dvbs2(const uint8_t *data, size_t len);

// CRC-32/ISO-HDLC (poly 0xEDB88320 reflected, init 0xFFFFFFFF, xorout
// 0xFFFFFFFF) -- guards the versioned cfg_t blob in NVS (spec 11.1).
uint32_t crc32_iso_hdlc(const uint8_t *data, size_t len);
