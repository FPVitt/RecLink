#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gopro_tlv.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    static uint8_t buf[1024];
    gopro_reasm_t r; gopro_reasm_init(&r, buf, sizeof buf);
    // round trip 1..1000 bytes
    for (int len = 1; len <= 1000; len++) {
        uint8_t body[1000]; for (int i = 0; i < len; i++) body[i] = (uint8_t)(i * 7 + len);
        gopro_tx_t t; gopro_tx_init(&t, body, len);
        uint8_t pkt[20]; size_t n; const uint8_t *out = NULL; uint16_t ol = 0; int got = 0;
        while ((n = gopro_tx_next(&t, pkt, sizeof pkt)) > 0) { if (gopro_reasm_feed(&r, pkt, n, &out, &ol)) got++; }
        CHECK(got == 1 && ol == len && memcmp(out, body, len) == 0);
    }
    // M3: lose the LAST continuation of a 40-byte message, then a complete 3-byte message arrives
    uint8_t m40[40]; for (int i = 0; i < 40; i++) m40[i] = (uint8_t)i;
    uint8_t m3[3] = { 0x93, 0x00, 0x05 };
    gopro_reasm_reset(&r);
    gopro_tx_t t; gopro_tx_init(&t, m40, 40);
    uint8_t pkt[20]; size_t n; const uint8_t *out = NULL; uint16_t ol = 0;
    uint8_t pk[4][20]; size_t pn[4]; int np = 0;
    while ((n = gopro_tx_next(&t, pkt, sizeof pkt)) > 0) { memcpy(pk[np], pkt, n); pn[np++] = n; }
    CHECK(np == 3);
    for (int i = 0; i < np - 1; i++) CHECK(!gopro_reasm_feed(&r, pk[i], pn[i], &out, &ol));   // last one "lost"
    gopro_tx_init(&t, m3, 3); n = gopro_tx_next(&t, pkt, sizeof pkt);
    int got = gopro_reasm_feed(&r, pkt, n, &out, &ol);
    CHECK(got == 1 && ol == 3 && memcmp(out, m3, 3) == 0);
    CHECK(r.drops == 1);
    // small buffer: oversize message is dropped, not truncated, and the next one still parses
    static uint8_t small[64]; gopro_reasm_t s; gopro_reasm_init(&s, small, sizeof small);
    uint8_t big[100]; memset(big, 1, sizeof big);
    gopro_tx_init(&t, big, 100); int g2 = 0;
    while ((n = gopro_tx_next(&t, pkt, sizeof pkt)) > 0) g2 += gopro_reasm_feed(&s, pkt, n, &out, &ol);
    CHECK(g2 == 0 && s.drops >= 1);
    gopro_tx_init(&t, m3, 3); n = gopro_tx_next(&t, pkt, sizeof pkt);
    CHECK(gopro_reasm_feed(&s, pkt, n, &out, &ol) == 1 && ol == 3);
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
