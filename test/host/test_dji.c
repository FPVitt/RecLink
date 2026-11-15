// DUML frame build / reassembly across BLE notifications.
#include "test.h"
#include "dji_duml.h"

static size_t build(uint8_t *out, size_t cap, uint16_t seq, uint8_t set, uint8_t id, const uint8_t *pl, uint16_t n)
{
    return duml_build(out, cap, 0x00, seq, set, id, pl, n);
}

static void test_round_trip(void)
{
    uint8_t f[DUML_MAX_FRAME], pl[5] = { 1, 2, 3, 4, 5 };
    size_t n = build(f, sizeof f, 0x1234, 0x1D, 0x03, pl, 5);
    CHECK_EQ(n, DUML_HDR_LEN + 2 + 5 + 4);
    CHECK_EQ(f[0], DUML_SOF);
    duml_reasm_t r; duml_reasm_reset(&r);
    duml_msg_t m;
    CHECK(duml_reasm_feed(&r, f, (uint16_t)n, &m));
    CHECK_EQ(m.seq, 0x1234); CHECK_EQ(m.cmd_set, 0x1D); CHECK_EQ(m.cmd_id, 0x03);
    CHECK_EQ(m.payload_len, 5); CHECK(memcmp(m.payload, pl, 5) == 0);
}

static void test_split_across_notifications(void)
{
    uint8_t f[DUML_MAX_FRAME], pl[40];
    for (int i = 0; i < 40; i++) { pl[i] = (uint8_t)(i * 3); }
    size_t n = build(f, sizeof f, 7, 0x00, 0x19, pl, 40);
    for (size_t chunk = 1; chunk <= 20; chunk++) {          // every plausible MTU-sized split
        duml_reasm_t r; duml_reasm_reset(&r);
        duml_msg_t m; int got = 0;
        for (size_t off = 0; off < n; off += chunk) {
            size_t k = (n - off < chunk) ? n - off : chunk;
            if (duml_reasm_feed(&r, f + off, (uint16_t)k, &m)) { got++; CHECK_EQ(m.payload_len, 40); }
        }
        CHECK_EQ(got, 1);
    }
}

static void test_garbage_and_crc(void)
{
    uint8_t f[DUML_MAX_FRAME], pl[3] = { 9, 8, 7 }, in[DUML_MAX_FRAME + 8];
    size_t n = build(f, sizeof f, 1, 0x1D, 0x02, pl, 3);
    duml_reasm_t r; duml_reasm_reset(&r);
    duml_msg_t m;
    // leading noise without SOF is skipped; the frame behind it still parses
    memcpy(in, "\x01\x02\x03\x04", 4); memcpy(in + 4, f, n);
    CHECK(duml_reasm_feed(&r, in, (uint16_t)(n + 4), &m));
    // a flipped payload bit fails the frame CRC32: no message, and the next good frame still parses
    uint8_t bad[DUML_MAX_FRAME]; memcpy(bad, f, n); bad[DUML_HDR_LEN + 3] ^= 0x10;
    duml_reasm_reset(&r);
    CHECK(!duml_reasm_feed(&r, bad, (uint16_t)n, &m));
    CHECK(duml_reasm_feed(&r, f, (uint16_t)n, &m));
    // a flipped header bit fails the header CRC16
    memcpy(bad, f, n); bad[8] ^= 0x01;
    duml_reasm_reset(&r);
    CHECK(!duml_reasm_feed(&r, bad, (uint16_t)n, &m));
}

static void test_two_frames_in_one_notification(void)
{
    uint8_t a[64], b[64], in[128], pa[2] = { 1, 1 }, pb[4] = { 2, 2, 2, 2 };
    size_t na = build(a, sizeof a, 1, 0x1D, 0x02, pa, 2);
    size_t nb = build(b, sizeof b, 2, 0x1D, 0x03, pb, 4);
    memcpy(in, a, na); memcpy(in + na, b, nb);
    duml_reasm_t r; duml_reasm_reset(&r);
    duml_msg_t m;
    CHECK(duml_reasm_feed(&r, in, (uint16_t)(na + nb), &m));
    CHECK_EQ(m.seq, 1); CHECK_EQ(m.payload_len, 2);
    CHECK(duml_reasm_next(&r, &m));
    CHECK_EQ(m.seq, 2); CHECK_EQ(m.payload_len, 4);
    CHECK(!duml_reasm_next(&r, &m));
}

static void test_limits(void)
{
    uint8_t out[DUML_MAX_FRAME], big[600] = { 0 };
    CHECK_EQ(duml_build(out, sizeof out, 0, 0, 0, 0, big, 300), 0);   // does not fit the buffer
    CHECK_EQ(duml_build(out, 8, 0, 0, 0, 0, NULL, 0), 0);             // output buffer too small
    // an impossible length field resets the reassembler instead of buffering forever
    duml_reasm_t r; duml_reasm_reset(&r);
    uint8_t evil[DUML_HDR_LEN] = { DUML_SOF, 0xFF, 0x03 };            // claims 1023 bytes
    duml_msg_t m;
    CHECK(!duml_reasm_feed(&r, evil, sizeof evil, &m));
    CHECK_EQ(r.have, 0);
    // a never-ending stream of SOF bytes cannot overflow the buffer
    uint8_t flood[1000]; memset(flood, DUML_SOF, sizeof flood);
    for (int i = 0; i < 20; i++) { duml_reasm_feed(&r, flood, sizeof flood, &m); }
    CHECK(r.have <= sizeof(r.buf));
}

int main(void)
{
    RUN(test_round_trip);
    RUN(test_split_across_notifications);
    RUN(test_garbage_and_crc);
    RUN(test_two_frames_in_one_notification);
    RUN(test_limits);
    TEST_MAIN_END();
}
