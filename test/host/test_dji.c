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

// ---- classic DUML (0x55) frames interleaved on the same notify link (Action 6 pushes 0D/02) ----
static const uint8_t k_duml_playback[] = { 0x55,0x11,0x04,0x92,0x02,0x01,0x00,0xa0,0x40,0x02,0x0c,0x01,0x01,0x00,0x01,0xb6,0x3b };
static const uint8_t k_duml_gimbal_hdr[] = { 0x55,0x3e,0x04,0x4b };    // header of a published 62-byte frame
static const uint8_t k_duml_mimo_hdr[]   = { 0x55,0x49,0x04,0x93 };    // header of a published 73-byte frame

static void test_duml55_crc8(void)
{
    // published frames (osmosis example, status_report, Mimo capture): the header CRC8 byte must match
    CHECK_EQ(duml55_crc8(k_duml_playback, 3), 0x92);
    CHECK_EQ(duml55_crc8(k_duml_gimbal_hdr, 3), 0x4b);
    CHECK_EQ(duml55_crc8(k_duml_mimo_hdr, 3), 0x93);
}

// A 0x55 frame whose body contains 0xAA bytes (even a plausible-looking R-SDK header), followed by a
// real R-SDK frame: the old reassembler synced on the inner 0xAA, failed a CRC, reset, and lost the
// real frame behind it.
static void test_duml55_with_aa_inside(void)
{
    uint8_t f55[40]; memset(f55, 0xAA, sizeof f55);
    f55[0] = 0x55; f55[1] = 40; f55[2] = 0x04; f55[3] = duml55_crc8(f55, 3);
    uint8_t good[64], pl[4] = { 1, 2, 3, 4 };
    size_t ng = build(good, sizeof good, 9, 0x1D, 0x02, pl, 4);
    uint8_t in[128]; memcpy(in, f55, 40); memcpy(in + 40, good, ng);
    for (size_t chunk = 1; chunk <= 60; chunk += 7) {       // whole, and split at awkward places
        duml_reasm_t r; duml_reasm_reset(&r);
        duml_msg_t m; int got = 0;
        size_t total = 40 + ng;
        for (size_t off = 0; off < total; off += chunk) {
            size_t k = (total - off < chunk) ? total - off : chunk;
            bool ok = duml_reasm_feed(&r, in + off, (uint16_t)k, &m);
            while (ok) { got++; CHECK_EQ(m.seq, 9); CHECK_EQ(m.cmd_id, 0x02); CHECK_EQ(m.payload_len, 4); ok = duml_reasm_next(&r, &m); }
        }
        CHECK_EQ(got, 1);
    }
    // the real published 0x55 frame is skipped whole, and the R-SDK frame after it still parses
    uint8_t in2[64]; memcpy(in2, k_duml_playback, sizeof k_duml_playback); memcpy(in2 + sizeof k_duml_playback, good, ng);
    duml_reasm_t r; duml_reasm_reset(&r); duml_msg_t m;
    CHECK(duml_reasm_feed(&r, in2, (uint16_t)(sizeof k_duml_playback + ng), &m));
    CHECK_EQ(m.seq, 9);
}

// A corrupt frame must cost one byte, not the buffer: the good frame right behind it survives.
static void test_bad_frame_does_not_eat_the_next(void)
{
    uint8_t a[64], b[64], in[160], pa[6] = { 5, 5, 5, 5, 5, 5 }, pb[2] = { 7, 7 };
    size_t na = build(a, sizeof a, 1, 0x1D, 0x02, pa, 6);
    size_t nb = build(b, sizeof b, 2, 0x1D, 0x02, pb, 2);
    a[DUML_HDR_LEN + 4] ^= 0x01;                           // payload CRC32 now fails
    memcpy(in, a, na); memcpy(in + na, b, nb);
    duml_reasm_t r; duml_reasm_reset(&r); duml_msg_t m;
    CHECK(duml_reasm_feed(&r, in, (uint16_t)(na + nb), &m));
    CHECK_EQ(m.seq, 2); CHECK_EQ(m.payload_len, 2);
    // a bogus 0xAA claiming a long frame is rejected by its header CRC at once, so it cannot sit on
    // the stream waiting for bytes that will never come
    uint8_t junk[16] = { 0xAA, 0xF0, 0x00, 1,2,3,4,5,6,7,8,9,10,11,12,13 };
    duml_reasm_reset(&r);
    memcpy(in, junk, sizeof junk); memcpy(in + sizeof junk, b, nb);
    CHECK(duml_reasm_feed(&r, in, (uint16_t)(sizeof junk + nb), &m));
    CHECK_EQ(m.seq, 2);
}

int main(void)
{
    RUN(test_round_trip);
    RUN(test_split_across_notifications);
    RUN(test_garbage_and_crc);
    RUN(test_two_frames_in_one_notification);
    RUN(test_limits);
    RUN(test_duml55_crc8);
    RUN(test_duml55_with_aa_inside);
    RUN(test_bad_frame_does_not_eat_the_next);
    TEST_MAIN_END();
}
