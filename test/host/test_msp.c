// MSPv2 codec (encode + streaming RX parser) and the response payload parsers.
#include "test.h"
#include "msp.h"
#include "msp_codec.h"
#include "msp_parse.h"

uint32_t now_ms(void) { return 12345; }     // stub for monoclock.c

static void to_response(uint8_t *frame) { frame[2] = '>'; }   // the CRC does not cover the direction byte

static void test_encode_known_frames(void)
{
    uint8_t out[16];
    size_t n = msp_encode(out, sizeof out, MSP_API_VERSION, NULL, 0);
    const uint8_t want_api[] = { '$', 'X', '<', 0x00, 0x01, 0x00, 0x00, 0x00, 0x45 };
    CHECK_EQ(n, sizeof want_api);
    CHECK(memcmp(out, want_api, sizeof want_api) == 0);

    n = msp_encode(out, sizeof out, MSP_STATUS, NULL, 0);
    const uint8_t want_status[] = { '$', 'X', '<', 0x00, 0x65, 0x00, 0x00, 0x00, 0xCA };
    CHECK_EQ(n, sizeof want_status);
    CHECK(memcmp(out, want_status, sizeof want_status) == 0);
}

static void test_encode_limits(void)
{
    uint8_t out[MSP_MAX_PAYLOAD + 16], pl[MSP_MAX_PAYLOAD + 1] = { 0 };
    CHECK_EQ(msp_encode(out, sizeof out, 1, pl, MSP_MAX_PAYLOAD), MSP_MAX_PAYLOAD + 9);
    CHECK_EQ(msp_encode(out, sizeof out, 1, pl, MSP_MAX_PAYLOAD + 1), 0);   // payload too large
    CHECK_EQ(msp_encode(out, 8, 1, NULL, 0), 0);                            // output buffer too small
    CHECK_EQ(msp_encode(out, 9, 1, NULL, 0), 9);                            // exactly fits
}

static void test_rx_round_trip_bytewise(void)
{
    uint8_t pl[40], f[64];
    for (int i = 0; i < 40; i++) { pl[i] = (uint8_t)(i * 5 + 1); }
    size_t n = msp_encode(f, sizeof f, 0x3007, pl, 40);      // a two-byte function id
    to_response(f);
    msp_rx_t rx; msp_rx_reset(&rx);
    msp_frame_t fr; bool crc = false; int got = 0;
    for (size_t i = 0; i < n; i++) { if (msp_rx_byte(&rx, f[i], &fr, &crc)) { got++; } }
    CHECK_EQ(got, 1); CHECK(!crc);
    CHECK_EQ(fr.func, 0x3007); CHECK_EQ(fr.len, 40); CHECK_EQ(fr.dir, '>');
    CHECK(memcmp(fr.payload, pl, 40) == 0);
}

static void test_rx_bad_crc_and_resync(void)
{
    uint8_t good[32], bad[32], pl[4] = { 1, 2, 3, 4 };
    size_t n = msp_encode(good, sizeof good, MSP_RC, pl, 4); to_response(good);
    memcpy(bad, good, n); bad[n - 2] ^= 0x01;                // corrupt one payload byte
    msp_rx_t rx; msp_rx_reset(&rx);
    msp_frame_t fr; bool crc = false; int got = 0;
    for (size_t i = 0; i < n; i++) { if (msp_rx_byte(&rx, bad[i], &fr, &crc)) { got++; } }
    CHECK_EQ(got, 0); CHECK(crc);                            // rejected, and reported as a CRC error
    crc = false;
    // garbage, then a good frame, then another good frame back to back
    const uint8_t noise[] = { 0x00, '$', 'M', 0xFF, '$', '$', 'X' };
    for (size_t i = 0; i < sizeof noise; i++) { msp_rx_byte(&rx, noise[i], &fr, &crc); }
    msp_rx_reset(&rx); crc = false; got = 0;
    for (int rep = 0; rep < 2; rep++) {
        for (size_t i = 0; i < n; i++) { if (msp_rx_byte(&rx, good[i], &fr, &crc)) { got++; } }
    }
    CHECK_EQ(got, 2); CHECK(!crc);
}

static void test_rx_oversize_rejected(void)
{
    // A length field above MSP_MAX_PAYLOAD must be refused before any byte is buffered.
    uint8_t hdr[] = { '$', 'X', '>', 0x00, 0x69, 0x00, 0x01, 0x01 /* len 257 */ };
    msp_rx_t rx; msp_rx_reset(&rx);
    msp_frame_t fr; bool crc = false;
    for (size_t i = 0; i < sizeof hdr; i++) { CHECK(!msp_rx_byte(&rx, hdr[i], &fr, &crc)); }
    for (int i = 0; i < 600; i++) { CHECK(!msp_rx_byte(&rx, 0xAA, &fr, &crc)); }   // stays out of the payload state
    uint8_t good[16]; size_t n = msp_encode(good, sizeof good, MSP_API_VERSION, NULL, 0); to_response(good);
    int got = 0;
    for (size_t i = 0; i < n; i++) { if (msp_rx_byte(&rx, good[i], &fr, &crc)) { got++; } }
    CHECK_EQ(got, 1);                                        // and the parser recovered
}

static msp_frame_t resp(uint16_t func, const uint8_t *pl, uint16_t len)
{
    msp_frame_t f; memset(&f, 0, sizeof f);
    f.func = func; f.len = len; f.dir = '>';
    if (len) { memcpy(f.payload, pl, len); }
    return f;
}

static void test_parse_api_version(void)
{
    fc_state_t s; memset(&s, 0, sizeof s);
    msp_parse_ctx_t ctx; msp_parse_ctx_init(&ctx, 6);
    uint8_t ok[3] = { 0, 1, 47 }, old[3] = { 0, 1, 46 }, newer[3] = { 0, 2, 0 };
    msp_frame_t f = resp(MSP_API_VERSION, ok, 3);
    CHECK(msp_parse_frame(&f, &s, &ctx)); CHECK(s.osd_supported);                 // 1.47: custom OSD messages exist
    f = resp(MSP_API_VERSION, old, 3);   msp_parse_frame(&f, &s, &ctx); CHECK(!s.osd_supported);
    f = resp(MSP_API_VERSION, newer, 3); msp_parse_frame(&f, &s, &ctx); CHECK(s.osd_supported);
    f = resp(MSP_API_VERSION, ok, 2);    CHECK(!msp_parse_frame(&f, &s, &ctx));   // short payload ignored
}

static void test_parse_arm_state(void)
{
    fc_state_t s; memset(&s, 0, sizeof s);
    msp_parse_ctx_t ctx; msp_parse_ctx_init(&ctx, 6);
    uint8_t status[25] = { 0 };
    status[4] = 0x08;                                        // sensors: GPS present
    // Before MSP_BOXIDS the arm bit index is unknown: armed must stay UNKNOWN, never "disarmed".
    status[6] = 0x02;                                        // flightModeFlags bit 1
    msp_frame_t f = resp(MSP_STATUS, status, 25);
    msp_parse_frame(&f, &s, &ctx);
    CHECK(!s.armed_valid); CHECK(s.gps_present);
    // BOXIDS: permanent id 0 (BOXARM) is at bit 1
    uint8_t boxids[4] = { 3, 0, 1, 2 };
    f = resp(MSP_BOXIDS, boxids, 4); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.arm_bit_index, 1);
    f = resp(MSP_STATUS, status, 25); msp_parse_frame(&f, &s, &ctx);
    CHECK(s.armed_valid); CHECK(s.armed);
    status[6] = 0x00;
    f = resp(MSP_STATUS, status, 25); msp_parse_frame(&f, &s, &ctx);
    CHECK(s.armed_valid); CHECK(!s.armed);
    // BOXARM missing from the list: stays unresolved
    uint8_t nobox[3] = { 3, 4, 5 };
    f = resp(MSP_BOXIDS, nobox, 3); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.arm_bit_index, -1);
}

static void test_parse_status_lengths(void)
{
    fc_state_t s; memset(&s, 0, sizeof s);
    msp_parse_ctx_t ctx; msp_parse_ctx_init(&ctx, 6);
    uint8_t boxids[1] = { 0 };
    msp_frame_t f = resp(MSP_BOXIDS, boxids, 1); msp_parse_frame(&f, &s, &ctx);
    uint8_t st[32] = { 0 };
    st[6] = 0x01;
    // too short: nothing is decoded
    f = resp(MSP_STATUS, st, 12); msp_parse_frame(&f, &s, &ctx); CHECK(!s.armed_valid);
    // armingDisableFlags forward walk: bc = p[15]; flags u32 at 17+bc
    memset(st, 0, sizeof st); st[6] = 0x01;
    st[15] = 0; st[16] = 0;  st[17] = 0x78; st[18] = 0x56; st[19] = 0x34; st[20] = 0x12;
    f = resp(MSP_STATUS, st, 25); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.arming_disable_flags, 0x12345678u);
    // a 15-byte frame has no p[15]: the flags must be left alone (previously a length off-by-one)
    s.arming_disable_flags = 0xDEADBEEF;
    f = resp(MSP_STATUS, st, 15); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.arming_disable_flags, 0xDEADBEEFu);
    // a byte-count that points past the end is ignored, not read
    st[15] = 200;
    f = resp(MSP_STATUS, st, 25); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.arming_disable_flags, 0xDEADBEEFu);
}

static void test_parse_variant_rc_and_errors(void)
{
    fc_state_t s; memset(&s, 0, sizeof s);
    msp_parse_ctx_t ctx; msp_parse_ctx_init(&ctx, 6);
    msp_frame_t f = resp(MSP_FC_VARIANT, (const uint8_t *)"BTFL", 4);
    CHECK(msp_parse_frame(&f, &s, &ctx)); CHECK(strcmp(s.fc_variant, "BTFL") == 0);
    uint8_t rc[12];
    for (int i = 0; i < 6; i++) { rc[i * 2] = (uint8_t)(1500 + i * 100); rc[i * 2 + 1] = (uint8_t)((1500 + i * 100) >> 8); }
    f = resp(MSP_RC, rc, 12); msp_parse_frame(&f, &s, &ctx);
    CHECK_EQ(s.rc_channel_count, 6); CHECK_EQ(s.rc[0], 1500); CHECK_EQ(s.rc[5], 2000);
    uint8_t big[200] = { 0 };
    f = resp(MSP_RC, big, 200); msp_parse_frame(&f, &s, &ctx);
    CHECK(s.rc_channel_count <= 18);                          // never past the rc[] array
    f = resp(MSP_STATUS, big, 25); f.dir = '!';               // an FC error reply changes nothing
    CHECK(!msp_parse_frame(&f, &s, &ctx));
}

int main(void)
{
    RUN(test_encode_known_frames);
    RUN(test_encode_limits);
    RUN(test_rx_round_trip_bytewise);
    RUN(test_rx_bad_crc_and_resync);
    RUN(test_rx_oversize_rejected);
    RUN(test_parse_api_version);
    RUN(test_parse_arm_state);
    RUN(test_parse_status_lengths);
    RUN(test_parse_variant_rc_and_errors);
    TEST_MAIN_END();
}
