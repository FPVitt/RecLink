// msp_codec.h -- pure MSPv2 frame encode/decode. No I/O, no allocation.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MSP_MAX_PAYLOAD 256

// Encode an MSPv2 request/command frame into `out` (must be >= MSP_MAX_PAYLOAD+9).
// dir is '<' request. Returns total frame length, or 0 on overflow.
size_t msp_encode(uint8_t *out, size_t out_cap,
                  uint16_t func, const uint8_t *payload, uint16_t len);

// ---- streaming RX parser ------------------------------------------------
typedef struct {
    uint16_t func;
    uint16_t len;
    uint8_t  flag;
    char     dir;            // '>' resp, '!' error
    uint8_t  payload[MSP_MAX_PAYLOAD];
} msp_frame_t;

typedef struct {
    uint8_t  state;
    uint8_t  crc;
    uint16_t idx;
    msp_frame_t f;
} msp_rx_t;

void msp_rx_reset(msp_rx_t *rx);

// Feed one byte. Returns true and fills *out when a valid frame completes.
// Returns false otherwise (incl. CRC/length errors -> *err_crc bumped).
bool msp_rx_byte(msp_rx_t *rx, uint8_t b, msp_frame_t *out, bool *err_crc);
