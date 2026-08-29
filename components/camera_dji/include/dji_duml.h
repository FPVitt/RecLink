// dji_duml.h -- DUML frame build/parse (spec 7.2). Little-endian.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DUML_SOF 0xAA
#define DUML_HDR_LEN 12          // SOF..CRC16 inclusive
#define DUML_MAX_FRAME 256

typedef struct {
    uint8_t  cmd_type;           // bit5: 0 cmd / 1 resp
    uint16_t seq;
    uint8_t  cmd_set;
    uint8_t  cmd_id;
    const uint8_t *payload;
    uint16_t payload_len;
} duml_msg_t;

// Build a full frame into out. cmd_type is caller-chosen (0x00 request).
// Returns total length or 0 on overflow.
size_t duml_build(uint8_t *out, size_t cap, uint8_t cmd_type, uint16_t seq,
                  uint8_t cmd_set, uint8_t cmd_id,
                  const uint8_t *payload, uint16_t payload_len);

// Streaming reassembly across BLE notifications.
typedef struct {
    uint8_t  buf[DUML_MAX_FRAME];
    uint16_t have;
    uint16_t need;
} duml_reasm_t;

void duml_reasm_reset(duml_reasm_t *r);

// Feed a notification. Returns true + fills *msg when a CRC-valid frame lands.
bool duml_reasm_feed(duml_reasm_t *r, const uint8_t *data, uint16_t len, duml_msg_t *msg);

// Pull the next already-buffered frame (call in a loop after feed for the case
// where one notification carried several frames).
bool duml_reasm_next(duml_reasm_t *r, duml_msg_t *msg);
