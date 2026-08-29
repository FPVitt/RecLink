#include "dji_duml.h"
#include "dji_crc.h"
#include <string.h>

// Frame (spec 7.2):
//  0    SOF   0xAA
//  1..2 Ver[15:10] | Length[9:0]   (total frame length, LE)
//  3    CmdType
//  4    ENC
//  5..7 RES = 0
//  8..9 SEQ (LE)
//  10..11 CRC-16 over [0..9] (LE)
//  12.. DATA = [CmdSet][CmdID][payload]
//  last 4  CRC-32 over [0 .. len-5] (LE)

size_t duml_build(uint8_t *out, size_t cap, uint8_t cmd_type, uint16_t seq,
                  uint8_t cmd_set, uint8_t cmd_id,
                  const uint8_t *payload, uint16_t payload_len)
{
    size_t total = DUML_HDR_LEN + 2 /*set+id*/ + payload_len + 4 /*crc32*/;
    if (total > cap || total > 0x3FF) { return 0; }

    memset(out, 0, DUML_HDR_LEN);
    out[0] = DUML_SOF;
    out[1] = (uint8_t)(total & 0xFF);
    out[2] = (uint8_t)((total >> 8) & 0x03);   // Ver = 0
    out[3] = cmd_type;
    out[4] = 0x00;                              // ENC none
    out[8] = (uint8_t)(seq & 0xFF);
    out[9] = (uint8_t)(seq >> 8);

    uint16_t c16 = dji_crc16(out, 10);
    out[10] = (uint8_t)(c16 & 0xFF);
    out[11] = (uint8_t)(c16 >> 8);

    size_t i = DUML_HDR_LEN;
    out[i++] = cmd_set;
    out[i++] = cmd_id;
    if (payload_len) { memcpy(out + i, payload, payload_len); i += payload_len; }

    uint32_t c32 = dji_crc32(out, i);
    out[i++] = (uint8_t)(c32 & 0xFF);
    out[i++] = (uint8_t)((c32 >> 8) & 0xFF);
    out[i++] = (uint8_t)((c32 >> 16) & 0xFF);
    out[i++] = (uint8_t)((c32 >> 24) & 0xFF);
    return i;
}

void duml_reasm_reset(duml_reasm_t *r) { r->have = 0; r->need = 0; r->consumed = 0; }

// Compact out the previously-returned frame. Deferred to here so the caller can
// finish reading msg->payload (which points into r->buf) before we clobber it.
static void compact(duml_reasm_t *r)
{
    if (!r->consumed) { return; }
    uint16_t leftover = (r->have > r->consumed) ? (r->have - r->consumed) : 0;
    memmove(r->buf, r->buf + r->consumed, leftover);
    r->have = leftover;
    r->consumed = 0;
}

static bool try_parse(duml_reasm_t *r, duml_msg_t *msg)
{
    compact(r);
    if (r->have < DUML_HDR_LEN) { return false; }
    uint16_t total = r->buf[1] | ((uint16_t)(r->buf[2] & 0x03) << 8);
    if (total < DUML_HDR_LEN + 6 || total > DUML_MAX_FRAME) {
        duml_reasm_reset(r);
        return false;
    }
    r->need = total;
    if (r->have < total) { return false; }

    // validate CRCs
    uint16_t c16 = dji_crc16(r->buf, 10);
    if ((c16 & 0xFF) != r->buf[10] || (c16 >> 8) != r->buf[11]) {
        duml_reasm_reset(r);
        return false;
    }
    uint32_t c32 = dji_crc32(r->buf, total - 4);
    const uint8_t *tc = &r->buf[total - 4];
    if ((c32 & 0xFF) != tc[0] || ((c32 >> 8) & 0xFF) != tc[1] ||
        ((c32 >> 16) & 0xFF) != tc[2] || ((c32 >> 24) & 0xFF) != tc[3]) {
        duml_reasm_reset(r);
        return false;
    }

    msg->cmd_type = r->buf[3];
    msg->seq = r->buf[8] | ((uint16_t)r->buf[9] << 8);
    msg->cmd_set = r->buf[12];
    msg->cmd_id = r->buf[13];
    msg->payload = &r->buf[14];
    msg->payload_len = total - DUML_HDR_LEN - 2 - 4;

    // Do NOT compact here -- msg->payload aliases r->buf[14..]. Mark the frame
    // consumed; compact() runs on the next try_parse, once the caller is done.
    r->consumed = total;
    r->need = 0;
    return true;
}

bool duml_reasm_feed(duml_reasm_t *r, const uint8_t *data, uint16_t len, duml_msg_t *msg)
{
    compact(r);   // clear any frame the previous call returned before appending
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (r->have == 0 && b != DUML_SOF) { continue; }   // resync on SOF
        if (r->have < sizeof(r->buf)) { r->buf[r->have++] = b; }
        else { duml_reasm_reset(r); }
    }
    return try_parse(r, msg);
}

bool duml_reasm_next(duml_reasm_t *r, duml_msg_t *msg)
{
    return try_parse(r, msg);
}
