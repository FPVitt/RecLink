// gopro_tlv.h -- Open GoPro packet header + TLV helpers (spec 6.3).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// GoPro packetizes every characteristic payload with its own length header:
//   general  : 1 byte,  bits[7:5]=00, bits[4:0]=len         (len <= 31)
//   ext-13   : 2 bytes, bits[7:5]=010, 13-bit len
//   ext-16   : 3 bytes, bits[7:5]=011, 16-bit len
//   continuation: 1 byte 0x80, follows for multi-packet payloads
// (spec 6.2). We only ever *send* short packets; we must *reassemble* on RX.

typedef struct {
    uint8_t  buf[1024];  // get-all-statuses can exceed 512 B
    uint16_t have;       // bytes accumulated
    uint16_t need;       // total expected (0 = idle)
} gopro_reasm_t;

void gopro_reasm_reset(gopro_reasm_t *r);

// Feed one raw notification. Returns true + sets *out/*out_len when a full
// logical payload is assembled (header stripped).
bool gopro_reasm_feed(gopro_reasm_t *r, const uint8_t *pkt, uint16_t len,
                      const uint8_t **out, uint16_t *out_len);

// Build an outbound short packet: [genhdr=len][body...]. Returns total length.
size_t gopro_pack(uint8_t *dst, size_t cap, const uint8_t *body, uint8_t body_len);
