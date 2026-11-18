// The hdg_aligned latch (spec 8.8.5): >= 5 m/s for >= 5 s with >= 5 satellites and a 3D fix;
// released on FC link loss or when the disarm hold (>= 3 s decoded disarm, not airborne) ends.
#include "test.h"
#include "hdg_align.h"

static hdg_align_in_t fly(uint16_t spd, uint8_t sats, uint32_t gps_rx)
{
    hdg_align_in_t in = { .link_up = true, .armed_valid = true, .armed = true, .airborne_hint = true,
                          .fix = 3, .sats = sats, .spd_cm_s = spd, .gps_rx_ms = gps_rx };
    return in;
}

static void test_latch(void)
{
    hdg_align_t h; hdg_align_reset(&h);
    // hover / slow: never
    for (uint32_t t = 1000; t < 20000; t += 500) { hdg_align_in_t in = fly(300, 12, t); CHECK(!hdg_align_step(&h, &in, t)); }
    // 5 m/s for 4.5 s: not yet; at 5 s: aligned
    uint32_t t = 20000;
    for (; t < 24500; t += 500) { hdg_align_in_t in = fly(500, 8, t); CHECK(!hdg_align_step(&h, &in, t)); }
    { hdg_align_in_t in = fly(500, 8, t); CHECK(!hdg_align_step(&h, &in, t)); }   // 24500: 4.5 s
    t = 25000; { hdg_align_in_t in = fly(500, 8, t); CHECK(hdg_align_step(&h, &in, t)); }
    // latched through hover afterwards
    t = 26000; { hdg_align_in_t in = fly(0, 8, t); CHECK(hdg_align_step(&h, &in, t)); }
    // link loss resets it
    { hdg_align_in_t in = fly(0, 8, t); in.link_up = false; CHECK(!hdg_align_step(&h, &in, t + 10)); }
}

static void test_conditions(void)
{
    hdg_align_t h; hdg_align_reset(&h);
    // 4 satellites: never (Betaflight does not correct yaw with <= 4)
    for (uint32_t t = 1000; t < 20000; t += 500) { hdg_align_in_t in = fly(1000, 4, t); CHECK(!hdg_align_step(&h, &in, t)); }
    // no 3D fix
    for (uint32_t t = 20000; t < 30000; t += 500) { hdg_align_in_t in = fly(1000, 9, t); in.fix = 0; CHECK(!hdg_align_step(&h, &in, t)); }
    // a dip below 5 m/s restarts the 5 s
    uint32_t t = 30000;
    for (; t < 34000; t += 500) { hdg_align_in_t in = fly(800, 9, t); hdg_align_step(&h, &in, t); }
    { hdg_align_in_t in = fly(400, 9, t); CHECK(!hdg_align_step(&h, &in, t)); t += 500; }
    for (int k = 0; k < 10; k++, t += 500) { hdg_align_in_t in = fly(800, 9, t); CHECK(!hdg_align_step(&h, &in, t)); }
    { hdg_align_in_t in = fly(800, 9, t); CHECK(hdg_align_step(&h, &in, t)); }
    // stale GPS breaks the run
    hdg_align_reset(&h);
    for (uint32_t k = 0; k < 20; k++) { hdg_align_in_t in = fly(900, 9, 1000); CHECK(!hdg_align_step(&h, &in, 3000 + k * 500)); }
}

static void test_disarm_hold(void)
{
    hdg_align_t h; hdg_align_reset(&h);
    uint32_t t = 1000;
    for (; t <= 7000; t += 500) { hdg_align_in_t in = fly(700, 9, t); hdg_align_step(&h, &in, t); }
    CHECK(h.aligned);
    // disarm: still latched for < 3 s, and while "maybe airborne"
    hdg_align_in_t in = fly(0, 9, t); in.armed = false; in.airborne_hint = true;
    CHECK(hdg_align_step(&h, &in, t)); CHECK(hdg_align_step(&h, &in, t + 5000));
    in.airborne_hint = false;
    CHECK(!hdg_align_step(&h, &in, t + 6000));                    // hold ended
    // a re-arm before 3 s keeps it
    hdg_align_reset(&h);
    for (t = 1000; t <= 7000; t += 500) { hdg_align_in_t f = fly(700, 9, t); hdg_align_step(&h, &f, t); }
    hdg_align_in_t d = fly(0, 9, t); d.armed = false; d.airborne_hint = false;
    CHECK(hdg_align_step(&h, &d, t)); CHECK(hdg_align_step(&h, &d, t + 2900));
    hdg_align_in_t a = fly(0, 9, t); CHECK(hdg_align_step(&h, &a, t + 2950));
    CHECK(hdg_align_step(&h, &d, t + 5000));                       // the hold restarted at the new disarm
    // arm state unknown (armed_valid false, link up): holds
    hdg_align_in_t u = fly(0, 9, t); u.armed_valid = false; u.armed = false;
    CHECK(hdg_align_step(&h, &u, t + 9000));
}

int main(void)
{
    RUN(test_latch);
    RUN(test_conditions);
    RUN(test_disarm_hold);
    TEST_MAIN_END();
}
