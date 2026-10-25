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

// Reassembly buffer sized for the largest message we ever ask for: a get-all
// response (0x12/0x13), which exceeds 512 B. Anything longer is dropped, not
// truncated -- `drops` counts that (and lost/stray packets) so a camera or
// firmware that sends something unexpectedly large is diagnosable instead of
// just appearing silent. Preset-status protobufs can run to several KB, but
// this firmware never requests them.
typedef struct {
    uint8_t  buf[1024];
    uint16_t have;       // bytes accumulated
    uint16_t need;       // total expected (0 = idle)
    uint8_t  counter;    // next expected 4-bit continuation counter
    uint16_t drops;      // messages discarded: overflow, lost or stray packet
} gopro_reasm_t;

void gopro_reasm_reset(gopro_reasm_t *r);

// Feed one raw notification. Returns true + sets *out/*out_len when a full
// logical payload is assembled (header stripped).
bool gopro_reasm_feed(gopro_reasm_t *r, const uint8_t *pkt, uint16_t len,
                      const uint8_t **out, uint16_t *out_len);

// ---- TX packetizer -------------------------------------------------------
// Open GoPro requires every message to be split into <= 20-byte packets, each
// carrying a header: a general 1-byte header for a body that fits in one
// packet, otherwise a 2-byte extended-13 header followed by continuation
// packets. (A 20..31-byte body would fit a general header's 5-bit length but
// NOT the 20-byte packet limit, so it must use extended-13 too.)
#define GOPRO_PKT_MAX 20

typedef struct {
    const uint8_t *body;
    uint16_t len;
    uint16_t sent;       // body bytes already emitted
    uint8_t  counter;    // 4-bit continuation counter
    bool     started;
} gopro_tx_t;

void gopro_tx_init(gopro_tx_t *t, const uint8_t *body, uint16_t len);

// Writes the next packet of the message into dst (cap >= GOPRO_PKT_MAX).
// Returns its length, or 0 when the whole body has been emitted.
size_t gopro_tx_next(gopro_tx_t *t, uint8_t *dst, size_t cap);
