#include "gopro_tlv.h"
#include <string.h>

void gopro_reasm_reset(gopro_reasm_t *r)
{
    r->have = 0; r->need = 0; r->counter = 0; r->drops = 0;
}

// Abandon the message being reassembled and count it. Keeps `drops` (unlike a
// full reset) so the driver can report that packets are going missing.
static void discard(gopro_reasm_t *r)
{
    r->have = 0; r->need = 0; r->counter = 0;
    if (r->drops != 0xFFFF) { r->drops++; }
}

bool gopro_reasm_feed(gopro_reasm_t *r, const uint8_t *pkt, uint16_t len,
                      const uint8_t **out, uint16_t *out_len)
{
    if (len == 0) { return false; }
    uint16_t i = 0;

    if (r->need == 0) {
        // start packet: parse the length header
        uint8_t b0 = pkt[0];
        if ((b0 & 0x80) != 0) {
            // Continuation with no start: the start packet was lost.
            discard(r);
            return false;
        }
        // verified: Open GoPro spec 2.0 "Packet Headers" (docs/ble/protocol/
        // data_protocol) @ gopro/OpenGoPro gh-pages 616bfb80 (2026-06-08).
        // byte0 bit7 = Start(0)/Continuation(1); bits[6:5] header type:
        //   00 General   -> bits[4:0] = 5-bit length
        //   01 Extended13 -> bits[4:0]<<8 | byte1 = 13-bit length
        //   10 Extended16 -> bits[4:0] reserved; byte1<<8 | byte2 = 16-bit length
        //   11 unused
        // (was checking 0x02/0x03 -- would drop every >20-byte response, e.g.
        //  get-all-statuses 0x13 and multi-status pushes.)
        uint8_t typ = (b0 >> 5) & 0x03;
        if (typ == 0x00) {                 // general, 5-bit
            r->need = b0 & 0x1F;
            i = 1;
        } else if (typ == 0x01) {          // extended 13-bit
            if (len < 2) { return false; }
            r->need = ((uint16_t)(b0 & 0x1F) << 8) | pkt[1];
            i = 2;
        } else if (typ == 0x02) {          // extended 16-bit
            if (len < 3) { return false; }
            r->need = ((uint16_t)pkt[1] << 8) | pkt[2];
            i = 3;
        } else {
            return false;
        }
        r->have = 0;
        r->counter = 0;
    } else {
        // Continuation packet: bit7 set, bits[3:0] = a counter that increments
        // per packet and wraps 0xF -> 0x0. A mismatch means a packet was lost;
        // concatenating what follows would silently produce a corrupt message
        // of the right length, so drop the whole thing and resync on the next
        // start packet.
        if ((pkt[0] & 0x80) == 0) { discard(r); return false; }
        if ((pkt[0] & 0x0F) != r->counter) { discard(r); return false; }
        r->counter = (uint8_t)((r->counter + 1) & 0x0F);
        i = 1;
    }

    uint16_t n = len - i;
    if (r->have + n > sizeof(r->buf)) { discard(r); return false; }
    memcpy(r->buf + r->have, pkt + i, n);
    r->have += n;

    if (r->have >= r->need) {
        *out = r->buf;
        *out_len = r->need;
        uint16_t done_need = r->need;
        r->need = 0;
        r->have = 0;
        (void)done_need;
        return true;
    }
    return false;
}

void gopro_tx_init(gopro_tx_t *t, const uint8_t *body, uint16_t len)
{
    t->body = body;
    t->len = len;
    t->sent = 0;
    t->counter = 0;
    t->started = false;
}

size_t gopro_tx_next(gopro_tx_t *t, uint8_t *dst, size_t cap)
{
    if (cap < GOPRO_PKT_MAX || !t->len || t->sent >= t->len) { return 0; }

    uint16_t left = (uint16_t)(t->len - t->sent);

    if (!t->started) {
        t->started = true;
        if (t->len <= GOPRO_PKT_MAX - 1) {
            // General header: 000LLLLL, the whole body in one packet.
            dst[0] = (uint8_t)t->len;
            memcpy(dst + 1, t->body, t->len);
            t->sent = t->len;
            return (size_t)t->len + 1;
        }
        // Extended-13 header: 001LLLLL LLLLLLLL, then as much body as fits.
        uint16_t n = GOPRO_PKT_MAX - 2;
        dst[0] = (uint8_t)(0x20 | (t->len >> 8));
        dst[1] = (uint8_t)(t->len & 0xFF);
        memcpy(dst + 2, t->body, n);
        t->sent = n;
        return (size_t)n + 2;
    }

    // Continuation: 1xxxCCCC, counter wraps 0xF -> 0x0.
    uint16_t n = left < GOPRO_PKT_MAX - 1 ? left : GOPRO_PKT_MAX - 1;
    dst[0] = (uint8_t)(0x80 | (t->counter & 0x0F));
    t->counter = (uint8_t)((t->counter + 1) & 0x0F);
    memcpy(dst + 1, t->body + t->sent, n);
    t->sent = (uint16_t)(t->sent + n);
    return (size_t)n + 1;
}
