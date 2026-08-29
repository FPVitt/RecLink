#include "gopro_tlv.h"
#include <string.h>

void gopro_reasm_reset(gopro_reasm_t *r) { r->have = 0; r->need = 0; }

bool gopro_reasm_feed(gopro_reasm_t *r, const uint8_t *pkt, uint16_t len,
                      const uint8_t **out, uint16_t *out_len)
{
    if (len == 0) { return false; }
    uint16_t i = 0;

    if (r->need == 0) {
        // start packet: parse the length header
        uint8_t b0 = pkt[0];
        if ((b0 & 0x80) != 0) {
            // unexpected continuation with no start; drop
            return false;
        }
        uint8_t typ = (b0 >> 5) & 0x03;
        if (typ == 0x00) {                 // general, 5-bit
            r->need = b0 & 0x1F;
            i = 1;
        } else if (typ == 0x02) {          // extended 13-bit
            if (len < 2) { return false; }
            r->need = ((uint16_t)(b0 & 0x1F) << 8) | pkt[1];
            i = 2;
        } else if (typ == 0x03) {          // extended 16-bit
            if (len < 3) { return false; }
            r->need = ((uint16_t)pkt[1] << 8) | pkt[2];
            i = 3;
        } else {
            return false;
        }
        r->have = 0;
    } else {
        // continuation packet: first byte must be 0x80
        if ((pkt[0] & 0x80) == 0) { gopro_reasm_reset(r); return false; }
        i = 1;
    }

    uint16_t n = len - i;
    if (r->have + n > sizeof(r->buf)) { gopro_reasm_reset(r); return false; }
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

size_t gopro_pack(uint8_t *dst, size_t cap, const uint8_t *body, uint8_t body_len)
{
    if (body_len > 0x1F || (size_t)(body_len + 1) > cap) { return 0; }
    dst[0] = body_len;                     // general header, type 00
    memcpy(dst + 1, body, body_len);
    return body_len + 1;
}
