#include "msp_codec.h"
#include "crc.h"
#include <string.h>

// Frame (spec 4.1):
//   $  X  <dir>  <flag>  <func_lo> <func_hi>  <len_lo> <len_hi>  <payload..>  <crc8>
//   crc8 = CRC-8/DVB-S2 over [flag .. last payload byte]

size_t msp_encode(uint8_t *out, size_t out_cap,
                  uint16_t func, const uint8_t *payload, uint16_t len)
{
    if (len > MSP_MAX_PAYLOAD) { return 0; }
    size_t total = 3 /*$X dir*/ + 5 /*flag func len*/ + len + 1 /*crc*/;
    if (out_cap < total) { return 0; }

    size_t i = 0;
    out[i++] = '$';
    out[i++] = 'X';
    out[i++] = '<';

    uint8_t hdr[5] = {
        0x00,                       // flag
        (uint8_t)(func & 0xFF),
        (uint8_t)(func >> 8),
        (uint8_t)(len & 0xFF),
        (uint8_t)(len >> 8),
    };
    uint8_t crc = 0x00;
    for (int k = 0; k < 5; k++) { out[i++] = hdr[k]; crc = crc8_dvbs2_update(crc, hdr[k]); }
    for (uint16_t k = 0; k < len; k++) { out[i++] = payload[k]; crc = crc8_dvbs2_update(crc, payload[k]); }
    out[i++] = crc;
    return i;
}

// ---- RX state machine --------------------------------------------------
enum { S_IDLE, S_X, S_DIR, S_FLAG, S_FN_LO, S_FN_HI, S_LEN_LO, S_LEN_HI, S_PAYLOAD, S_CRC };

void msp_rx_reset(msp_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
    rx->state = S_IDLE;
}

bool msp_rx_byte(msp_rx_t *rx, uint8_t b, msp_frame_t *out, bool *err_crc)
{
    switch (rx->state) {
    case S_IDLE:
        if (b == '$') { rx->state = S_X; }
        break;
    case S_X:
        rx->state = (b == 'X') ? S_DIR : S_IDLE;
        break;
    case S_DIR:
        rx->f.dir = (char)b;
        rx->state = (b == '>' || b == '!' || b == '<') ? S_FLAG : S_IDLE;
        break;
    case S_FLAG:
        rx->f.flag = b;
        rx->crc = crc8_dvbs2_update(0x00, b);
        rx->state = S_FN_LO;
        break;
    case S_FN_LO:
        rx->f.func = b;
        rx->crc = crc8_dvbs2_update(rx->crc, b);
        rx->state = S_FN_HI;
        break;
    case S_FN_HI:
        rx->f.func |= (uint16_t)b << 8;
        rx->crc = crc8_dvbs2_update(rx->crc, b);
        rx->state = S_LEN_LO;
        break;
    case S_LEN_LO:
        rx->f.len = b;
        rx->crc = crc8_dvbs2_update(rx->crc, b);
        rx->state = S_LEN_HI;
        break;
    case S_LEN_HI:
        rx->f.len |= (uint16_t)b << 8;
        rx->crc = crc8_dvbs2_update(rx->crc, b);
        if (rx->f.len > MSP_MAX_PAYLOAD) {   // overflow -> reject, never allocate
            rx->state = S_IDLE;
            break;
        }
        rx->idx = 0;
        rx->state = rx->f.len ? S_PAYLOAD : S_CRC;
        break;
    case S_PAYLOAD:
        rx->f.payload[rx->idx++] = b;
        rx->crc = crc8_dvbs2_update(rx->crc, b);
        if (rx->idx >= rx->f.len) { rx->state = S_CRC; }
        break;
    case S_CRC:
        rx->state = S_IDLE;
        if (b == rx->crc) {
            *out = rx->f;
            return true;
        }
        if (err_crc) { *err_crc = true; }
        break;
    default:
        rx->state = S_IDLE;
        break;
    }
    return false;
}
