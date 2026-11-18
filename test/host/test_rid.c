// Remote ID receiver (spec 8.8) pure logic: the two callback matchers (incl. fuzz with lying
// lengths / truncation / wrong OUI), pack splitting, Basic ID + Location decode into the table,
// 0/0 and INV_ALT, merge-on-ID, eviction, geometry (incl. +-180 deg and the antimeridian), the
// 255 m/s jump rejection, levels + two-sample ALERT + hysteresis, the GPS gate, the arrow glyph
// mapping, and every %rp render worst case (<= 16 characters, imperial included).
#include "test.h"
#include "rid_core.h"
#include "opendroneid.h"

#include <math.h>

static const uint8_t A1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t A2[6] = { 0x21, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t W1[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };

// ---- message builders (core-c's own encoders) ------------------------------------------------
static void enc_basic(uint8_t out[25], const char *id, int type)
{
    ODID_BasicID_data d; odid_initBasicIDData(&d);
    d.IDType = (ODID_idtype_t)type; d.UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    strncpy(d.UASID, id, sizeof(d.UASID) - 1);
    ODID_BasicID_encoded e; CHECK_EQ(encodeBasicIDMessage(&e, &d), ODID_SUCCESS);
    memcpy(out, &e, 25);
}
static void enc_loc(uint8_t out[25], double lat, double lon, float alt, int status)
{
    ODID_Location_data d; odid_initLocationData(&d);
    d.Status = (ODID_status_t)status; d.Latitude = lat; d.Longitude = lon; d.AltitudeGeo = alt;
    ODID_Location_encoded e; CHECK_EQ(encodeLocationMessage(&e, &d), ODID_SUCCESS);
    memcpy(out, &e, 25);
}
// A Location message with raw (possibly hostile) int32 lat/lon written straight into the bytes.
static void raw_loc(uint8_t out[25], int32_t lat, int32_t lon)
{
    enc_loc(out, 1.0, 1.0, 10.0f, 2);
    memcpy(out + 5, &lat, 4); memcpy(out + 9, &lon, 4);
}
static size_t ble_adv(uint8_t *adv, const uint8_t msg[25])
{
    size_t n = 0;
    adv[n++] = 0x1E; adv[n++] = 0x16; adv[n++] = 0xFA; adv[n++] = 0xFF; adv[n++] = 0x0D; adv[n++] = 7;
    memcpy(adv + n, msg, 25); n += 25;
    return n;                                        // 31
}
static size_t make_pack(uint8_t *p, const uint8_t (*msgs)[25], int n)
{
    p[0] = 0xF2; p[1] = 25; p[2] = (uint8_t)n;
    for (int i = 0; i < n; i++) { memcpy(p + 3 + 25 * i, msgs[i], 25); }
    return 3 + 25 * (size_t)n;
}
static size_t beacon(uint8_t *f, const uint8_t *pack, size_t plen, bool ssid_first)
{
    size_t n = 0;
    memset(f, 0, 36);
    f[0] = 0x80; memset(f + 4, 0xFF, 6); memcpy(f + 10, W1, 6); memcpy(f + 16, W1, 6);
    n = 36;
    if (ssid_first) { f[n++] = 0; f[n++] = 4; memcpy(f + n, "RIDX", 4); n += 4; f[n++] = 1; f[n++] = 1; f[n++] = 0x8C; }
    f[n++] = 0xDD; f[n++] = (uint8_t)(3 + 1 + 1 + plen);
    f[n++] = 0xFA; f[n++] = 0x0B; f[n++] = 0xBC; f[n++] = 0x0D; f[n++] = 3;   // counter
    memcpy(f + n, pack, plen); n += plen;
    if (!ssid_first) { f[n++] = 0; f[n++] = 0; }
    return n;
}
static size_t nan_sdf(uint8_t *f, const uint8_t *pack, size_t plen)
{
    static const uint8_t da[6] = { 0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00 };
    static const uint8_t svc[6] = { 0x88, 0x69, 0x19, 0x9D, 0x92, 0x09 };
    size_t n = 0;
    memset(f, 0, 24);
    f[0] = 0xD0; memcpy(f + 4, da, 6); memcpy(f + 10, W1, 6);
    n = 24;
    f[n++] = 0x04; f[n++] = 0x09; f[n++] = 0x50; f[n++] = 0x6F; f[n++] = 0x9A; f[n++] = 0x13;
    size_t sil = 1 + plen;
    f[n++] = 0x03; f[n++] = (uint8_t)((10 + sil) & 0xFF); f[n++] = (uint8_t)((10 + sil) >> 8);
    memcpy(f + n, svc, 6); n += 6;
    f[n++] = 0x01; f[n++] = 0x00; f[n++] = 0x10; f[n++] = (uint8_t)sil;
    f[n++] = 9;                                      // message counter
    memcpy(f + n, pack, plen); n += plen;
    f[n++] = 0x0E; f[n++] = 4; f[n++] = 0; f[n++] = 1; f[n++] = 0x00; f[n++] = 0x02; f[n++] = 0;
    return n;
}

static uint32_t g_seed = 12345;
static uint32_t rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

// ================================================================================================
static void test_ble_match(void)
{
    uint8_t m[25]; enc_loc(m, 47.0, 8.0, 100.0f, 2);
    uint8_t adv[64]; size_t n = ble_adv(adv, m);
    const uint8_t *got = NULL;
    CHECK(rid_ble_match(adv, n, &got));
    CHECK(got == adv + 6);
    // preceded by a flags AD structure (legal in a longer PDU; must still be found)
    uint8_t adv2[64] = { 0x02, 0x01, 0x06 }; memcpy(adv2 + 3, adv, n);
    CHECK(rid_ble_match(adv2, n + 3, &got)); CHECK(got == adv2 + 9);
    // wrong UUID / app code / AD type
    uint8_t b[64];
    memcpy(b, adv, n); b[2] = 0xFB; CHECK(!rid_ble_match(b, n, &got));
    memcpy(b, adv, n); b[4] = 0x0C; CHECK(!rid_ble_match(b, n, &got));
    memcpy(b, adv, n); b[1] = 0x09; CHECK(!rid_ble_match(b, n, &got));
    // truncated by one byte, and a length field shorter than a message
    CHECK(!rid_ble_match(adv, n - 1, &got));
    memcpy(b, adv, n); b[0] = 0x1D; CHECK(!rid_ble_match(b, n, &got));
    // lying length running past the buffer
    memcpy(b, adv, n); b[0] = 0xFF; CHECK(!rid_ble_match(b, n, &got));
    CHECK(!rid_ble_match(NULL, 5, &got)); CHECK(!rid_ble_match(adv, 0, &got));
}

static void test_ble_match_fuzz(void)
{
    // Random bytes of every length 0..31 (ASan catches any over-read), plus mutated real adverts.
    uint8_t m[25]; enc_basic(m, "SN123", ODID_IDTYPE_SERIAL_NUMBER);
    uint8_t good[31]; ble_adv(good, m);
    int hits = 0;
    for (int it = 0; it < 200000; it++) {
        size_t len = rnd() % 32;
        uint8_t *buf = malloc(len ? len : 1);
        if (it & 1) { for (size_t i = 0; i < len; i++) { buf[i] = (uint8_t)rnd(); } }
        else { memcpy(buf, good, len); if (len) { buf[rnd() % len] = (uint8_t)rnd(); } }
        const uint8_t *got = NULL;
        if (rid_ble_match(buf, len, &got)) { hits++; CHECK(got >= buf && got + 25 <= buf + len); }
        free(buf);
    }
    CHECK(hits > 0);
}

static void test_pack_split(void)
{
    uint8_t ms[9][25]; for (int i = 0; i < 9; i++) { enc_loc(ms[i], 1.0 + i, 2.0, 0, 2); }
    uint8_t p[300]; const uint8_t *out[9];
    size_t n = make_pack(p, ms, 3);
    CHECK_EQ(rid_pack_split(p, n, out), 3); CHECK(out[2] == p + 3 + 50);
    CHECK_EQ(rid_pack_split(p, n - 1, out), -1);               // truncated
    p[2] = 10; CHECK_EQ(rid_pack_split(p, 300, out), -1);       // count > 9
    p[2] = 0;  CHECK_EQ(rid_pack_split(p, 300, out), -1);       // count 0
    p[2] = 3; p[1] = 24; CHECK_EQ(rid_pack_split(p, n, out), -1);   // wrong message size
    p[1] = 25; p[0] = 0x12; CHECK_EQ(rid_pack_split(p, n, out), -1); // not a pack type
    n = make_pack(p, ms, 9); CHECK_EQ(n, 228u); CHECK_EQ(rid_pack_split(p, n, out), 9);
    CHECK_EQ(rid_pack_split(p, 2, out), -1);
}

static void test_wifi_match(void)
{
    uint8_t ms[2][25]; enc_basic(ms[0], "SNWIFI1", 1); enc_loc(ms[1], 47.0, 8.0, 50.0f, 2);
    uint8_t pk[64]; size_t pl = make_pack(pk, ms, 2);
    uint8_t f[400], src[6]; const uint8_t *pp; size_t plen;

    size_t n = beacon(f, pk, pl, true);
    CHECK_EQ(rid_wifi_match(f, n, src, &pp, &plen), RID_WIFI_KIND_BEACON);
    CHECK_EQ(plen, pl); CHECK(memcmp(src, W1, 6) == 0); CHECK(memcmp(pp, pk, pl) == 0);
    n = beacon(f, pk, pl, false);                               // vendor IE first
    CHECK_EQ(rid_wifi_match(f, n, src, &pp, &plen), RID_WIFI_KIND_BEACON);
    // wrong OUI / type, truncated IE, lying IE length, pack count lying
    uint8_t g[400];
    memcpy(g, f, n); g[38] = 0xFB; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);
    memcpy(g, f, n); g[41] = 0x0C; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);
    CHECK_EQ(rid_wifi_match(f, n - 4, src, &pp, &plen), RID_WIFI_KIND_NONE);
    memcpy(g, f, n); g[37] = 0xFF; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);
    memcpy(g, f, n); g[42 + 2] = 9; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);
    // not a beacon: a data frame / probe response with the same body
    memcpy(g, f, n); g[0] = 0x08; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);
    memcpy(g, f, n); g[0] = 0x50; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);

    n = nan_sdf(f, pk, pl);
    CHECK_EQ(rid_wifi_match(f, n, src, &pp, &plen), RID_WIFI_KIND_NAN);
    CHECK_EQ(plen, pl); CHECK(memcmp(pp, pk, pl) == 0);
    memcpy(g, f, n); g[9] = 0x01; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE);  // DA
    memcpy(g, f, n); g[33] = 0x00; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE); // service id
    memcpy(g, f, n); g[42] = 0xFF; CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE); // SIL lies
    memcpy(g, f, n); g[31] ^= 1;   CHECK_EQ(rid_wifi_match(g, n, src, &pp, &plen), RID_WIFI_KIND_NONE); // attr len
    for (size_t k = 0; k < 24 + 6 + 13 + 1 + pl; k++) {        // every truncation
        CHECK_EQ(rid_wifi_match(f, k, src, &pp, &plen), RID_WIFI_KIND_NONE);
    }
}

static void test_wifi_match_fuzz(void)
{
    uint8_t ms[3][25]; enc_basic(ms[0], "FUZZ", 1); enc_loc(ms[1], 1, 1, 0, 2); enc_loc(ms[2], 2, 2, 0, 2);
    uint8_t pk[100]; size_t pl = make_pack(pk, ms, 3);
    uint8_t good_b[400], good_n[400];
    size_t nb = beacon(good_b, pk, pl, true), nn = nan_sdf(good_n, pk, pl);
    int hits = 0;
    for (int it = 0; it < 200000; it++) {
        size_t len = rnd() % 400;
        uint8_t *buf = malloc(len ? len : 1);
        int mode = it % 3;
        if (mode == 0) { for (size_t i = 0; i < len; i++) { buf[i] = (uint8_t)rnd(); } buf[0] = (it & 4) ? 0x80 : 0xD0; }
        else {
            const uint8_t *src0 = mode == 1 ? good_b : good_n; size_t sl = mode == 1 ? nb : nn;
            for (size_t i = 0; i < len; i++) { buf[i] = i < sl ? src0[i] : (uint8_t)rnd(); }
            for (int m2 = 0; m2 < 3 && len; m2++) { buf[rnd() % len] = (uint8_t)rnd(); }
        }
        uint8_t src[6]; const uint8_t *pp = NULL; size_t plen = 0;
        int k = rid_wifi_match(buf, len, src, &pp, &plen);
        if (k != RID_WIFI_KIND_NONE) {
            hits++;
            CHECK(pp >= buf && pp + plen <= buf + len);
            CHECK(plen >= 28 && plen <= 228 && (plen - 3) % 25 == 0);
            const uint8_t *mm[9]; CHECK(rid_pack_split(pp, plen, mm) >= 1);
        }
        free(buf);
    }
    CHECK(hits > 0);
}

// ================================================================================================
static rid_own_t OWN = { true, 470000000, 80000000, false, 0 };   // 47.0 N, 8.0 E

static void test_table_basic(void)
{
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    enc_basic(m, "SERIAL-0001", ODID_IDTYPE_SERIAL_NUMBER);
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1000);
    CHECK(i >= 0); CHECK(t.ac[i].id_valid); CHECK(t.ac[i].id_printable);
    CHECK(strcmp(t.ac[i].uas_id, "SERIAL-0001") == 0); CHECK_EQ(t.ac[i].uas_id[20], 0);
    enc_loc(m, 47.001, 8.0, 120.0f, RID_STATUS_AIRBORNE);
    CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1100), i);
    CHECK(t.ac[i].pos_valid); CHECK_EQ(t.ac[i].lat_1e7, 470010000);
    // 0/0 = unknown -> unconfirmed
    enc_loc(m, 0.0, 0.0, 10.0f, 2);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1200);
    CHECK(!t.ac[i].pos_valid);
    enc_loc(m, 47.001, 8.0, (float)INV_ALT, 2);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1300);
    CHECK(t.ac[i].pos_valid);
    // Operator ID / Self-ID / System / Auth: discarded unparsed (no entry created)
    uint8_t other[25] = { 0x52 }; CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A2, other, &OWN, 1400), -1);
    other[0] = 0x32; CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A2, other, &OWN, 1400), -1);
    other[0] = 0x22; CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A2, other, &OWN, 1400), -1);
    other[0] = 0xF2; CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A2, other, &OWN, 1400), -1);
    int used = 0; for (int k = 0; k < RID_MAX_AC; k++) { used += t.ac[k].used; }
    CHECK_EQ(used, 1);
}

static void test_table_uasid_unterminated(void)
{
    // A UUID-type ID (not serial/CAA): core-c memcpy's 21 bytes; we must force [20] = 0.
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25]; enc_basic(m, "", ODID_IDTYPE_UTM_ASSIGNED_UUID);
    for (int k = 2; k < 22; k++) { m[k] = 0xAB; }               // 20 ID bytes, no NUL
    m[22] = 0x41;                                                // reserved byte right after
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 10);
    CHECK(i >= 0); CHECK_EQ(t.ac[i].uas_id[20], 0); CHECK(!t.ac[i].id_printable);
}

static void test_table_range_reject(void)
{
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    raw_loc(m, 900000001, 80000000);  CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 10), -1);
    raw_loc(m, 470000000, 1800000001); CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 10), -1);
    raw_loc(m, INT32_MIN, INT32_MAX);  CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 10), -1);
    CHECK_EQ(t.range_rejects, 3u);
    raw_loc(m, -900000000, -1800000000); CHECK(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 10) >= 0);
}

static void test_table_merge_dual(void)
{
    // One aircraft on BLE and Wi-Fi (different addresses): two entries until the ID merges them.
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    enc_loc(m, 47.001, 8.0, 0, 2);
    int b = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 100);
    uint8_t ms[2][25]; enc_basic(ms[0], "DUAL-1", 1); enc_loc(ms[1], 47.0011, 8.0, 0, 2);
    uint8_t pk[64]; size_t pl = make_pack(pk, ms, 2);
    CHECK_EQ(rid_table_ingest_pack(&t, W1, pk, pl, &OWN, 200), 2);
    int used = 0; for (int k = 0; k < RID_MAX_AC; k++) { used += t.ac[k].used; }
    CHECK_EQ(used, 2);
    enc_basic(m, "DUAL-1", 1);
    int b2 = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 300);
    CHECK_EQ(b2, b);
    used = 0; for (int k = 0; k < RID_MAX_AC; k++) { used += t.ac[k].used; }
    CHECK_EQ(used, 1);
    CHECK_EQ(t.ac[b].src_mask, 3);
    CHECK_EQ(t.ac[b].lat_1e7, 470011000);                       // the newer (Wi-Fi) position kept
    // later BLE and Wi-Fi traffic both land on the merged entry
    enc_loc(m, 47.0012, 8.0, 0, 2);
    CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 400), b);
    CHECK_EQ(rid_table_ingest_pack(&t, W1, pk, pl, &OWN, 500), 2);
    used = 0; for (int k = 0; k < RID_MAX_AC; k++) { used += t.ac[k].used; }
    CHECK_EQ(used, 1);
    rid_params_t p = { 1000, 300, 100, 5, 0, NULL, 0 };
    rid_pick_t pk2; rid_table_update(&t, &p, &OWN, 600, &pk2);
    CHECK_EQ(pk2.count, 1);                                      // counted once, not RID2
}

static void test_table_eviction(void)
{
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25], a[6] = { 1, 2, 3, 4, 5, 0 };
    for (int k = 0; k < RID_MAX_AC; k++) {                      // 8 aircraft at 100 m .. 800 m north
        a[5] = (uint8_t)k;
        enc_loc(m, 47.0 + 0.0009 * (k + 1), 8.0, 0, 2);
        CHECK(rid_table_ingest_msg(&t, RID_TR_BLE, a, m, &OWN, 1000) >= 0);
    }
    rid_params_t p = { 1000, 300, 100, 5, 0, NULL, 0 };
    rid_pick_t pk; rid_table_update(&t, &p, &OWN, 1000, &pk);
    // a newcomer farther than all -> dropped; a newcomer with no position -> dropped
    a[5] = 50; enc_loc(m, 47.05, 8.0, 0, 2);
    CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, a, m, &OWN, 1100), -1);
    enc_basic(m, "NEW", 1); CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, a, m, &OWN, 1100), -1);
    // a nearer newcomer evicts the farthest (800 m)
    enc_loc(m, 47.0001, 8.0, 0, 2);
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, a, m, &OWN, 1100);
    CHECK(i >= 0);
    bool far_gone = true;
    for (int k = 0; k < RID_MAX_AC; k++) { if (t.ac[k].used && t.ac[k].addr[0][5] == 7) { far_gone = false; } }
    CHECK(far_gone);
    // a stale entry goes first: after 11 s with no Location everyone is stale -> oldest evicted
    a[5] = 51; enc_loc(m, 47.0002, 8.0, 0, 2);
    CHECK(rid_table_ingest_msg(&t, RID_TR_BLE, a, m, &OWN, 12500) >= 0);
    CHECK(t.evict_drops >= 2u);
}

static void test_jump_reject(void)
{
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    enc_loc(m, 47.001, 8.0, 0, 2);
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1000);
    // 1 s later, 1.1 km away: 1100 m/s -> rejected, previous kept
    enc_loc(m, 47.011, 8.0, 0, 2);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 2000);
    CHECK_EQ(t.ac[i].lat_1e7, 470010000); CHECK_EQ(t.jump_rejects, 1u);
    // 100 m in 1 s (100 m/s) -> accepted
    enc_loc(m, 47.0019, 8.0, 0, 2);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 3000);
    CHECK_EQ(t.ac[i].lat_1e7, 470019000);
    // a hostile far point (> 20 km, < 78 s): rejected
    enc_loc(m, 48.0, 8.0, 0, 2);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 3100);
    CHECK_EQ(t.ac[i].lat_1e7, 470019000);
}

// ================================================================================================
static void test_geo(void)
{
    uint32_t d; uint16_t b;
    CHECK(rid_geo(470000000, 80000000, 470009000, 80000000, &d, &b));   // 0.0009 deg N = 100.08 m
    CHECK_EQ(d, 100); CHECK_EQ(b, 0);
    CHECK(rid_geo(470000000, 80000000, 470000000, 80013170, &d, &b));   // east, cos(47) = 0.682
    CHECK(d >= 99 && d <= 101); CHECK_EQ(b, 90);
    CHECK(rid_geo(470000000, 80000000, 469991000, 80000000, &d, &b)); CHECK_EQ(b, 180);
    CHECK(rid_geo(470000000, 80000000, 470000000, 79986830, &d, &b)); CHECK_EQ(b, 270);
    CHECK(rid_geo(0, 10, 9000, 9010, &d, &b)); CHECK_EQ(b, 45);
    // 20 km gate (before trig) and hostile values
    CHECK(!rid_geo(470000000, 80000000, 471800000, 80000000, &d, &b));
    CHECK(!rid_geo(470000000, 80000000, -900000000, -1800000000, &d, &b));
    CHECK(!rid_geo(470000000, 80000000, 0, 0, &d, &b));                 // unknown
    CHECK(!rid_geo(0, 0, 470000000, 80000000, &d, &b));
    // antimeridian: 179.9999 E to 179.9999 W at the equator is ~22 m, bearing east
    CHECK(rid_geo(1, 1799999000, 1, -1799999000, &d, &b));
    CHECK(d >= 21 && d <= 23); CHECK_EQ(b, 90);
    CHECK(rid_latlon_ok(900000000, 1800000000)); CHECK(!rid_latlon_ok(900000001, 0));
}

static void test_arrow(void)
{
    // Betaflight mapping: dead ahead = NORTH 0x68, right = EAST 0x64, behind = SOUTH 0x60, left = WEST 0x6C
    CHECK_EQ(rid_arrow_glyph(0, 0), 0x68);
    CHECK_EQ(rid_arrow_glyph(90, 0), 0x64);
    CHECK_EQ(rid_arrow_glyph(180, 0), 0x60);
    CHECK_EQ(rid_arrow_glyph(270, 0), 0x6C);
    CHECK_EQ(rid_arrow_glyph(90, 90), 0x68);          // heading east, target east -> ahead
    CHECK_EQ(rid_arrow_glyph(0, 90), 0x6C);           // heading east, target north -> left
    CHECK_EQ(rid_arrow_glyph(10, 350), rid_arrow_glyph(20, 0));
    // every relative bearing against the spec formula d = round(rel/22.5) mod 16
    for (int brg = 0; brg < 360; brg++) {
        for (int hdg = 0; hdg < 360; hdg += 7) {
            int rel = ((brg - hdg) % 360 + 360) % 360;
            int d = (int)floor(rel / 22.5 + 0.5) % 16;
            int want = 0x60 + ((16 - d + 8) % 16);
            uint8_t g = rid_arrow_glyph((uint16_t)brg, (int16_t)hdg);
            CHECK(g >= 0x60 && g <= 0x6F);
            if (g != want) { CHECK_EQ(g, want); }
        }
    }
    CHECK(strcmp(rid_compass8(0), "N") == 0); CHECK(strcmp(rid_compass8(44), "NE") == 0);
    CHECK(strcmp(rid_compass8(359), "N") == 0); CHECK(strcmp(rid_compass8(200), "S") == 0);
    CHECK(strcmp(rid_compass8(300), "NW") == 0);
}

// ================================================================================================
static rid_params_t P = { 1000, 300, 100, 5, 0, NULL, 0 };

static void put_ac(rid_table_t *t, int idx, double north_m, uint32_t now, const char *id, int status)
{
    uint8_t a[6] = { 9, 9, 9, 9, 9, (uint8_t)idx }, m[25];
    if (id) { enc_basic(m, id, 1); rid_table_ingest_msg(t, RID_TR_BLE, a, m, &OWN, now); }
    enc_loc(m, 47.0 + north_m / 111195.0, 8.0, 50, status);
    rid_table_ingest_msg(t, RID_TR_BLE, a, m, &OWN, now);
}

static void test_levels_and_caps(void)
{
    rid_table_t t; rid_table_clear(&t); rid_pick_t pk;
    put_ac(&t, 0, 250, 1000, "ID0", 2);
    rid_table_update(&t, &P, &OWN, 1000, &pk);
    CHECK(pk.have); CHECK_EQ(pk.level, RID_LVL_WARN); CHECK(pk.dist_m >= 249 && pk.dist_m <= 251);
    // un-identified: capped at NOTICE for 15 s
    rid_table_clear(&t); put_ac(&t, 1, 50, 1000, NULL, 2);
    rid_table_update(&t, &P, &OWN, 1000, &pk); CHECK_EQ(pk.level, RID_LVL_NOTICE); CHECK(!pk.alert_ok);
    put_ac(&t, 1, 50, 16100, NULL, 2);
    rid_table_update(&t, &P, &OWN, 16100, &pk); CHECK_EQ(pk.level, RID_LVL_ALERT);
    // ground status capped unless warn.ground
    rid_table_clear(&t); put_ac(&t, 2, 50, 1000, "G", RID_STATUS_GROUND);
    rid_table_update(&t, &P, &OWN, 1000, &pk); CHECK_EQ(pk.level, RID_LVL_NOTICE);
    rid_params_t pg = P; pg.ground = 1;
    rid_table_update(&t, &pg, &OWN, 1000, &pk); CHECK_EQ(pk.level, RID_LVL_ALERT);
    // ignore list
    char ign[4][21] = { "G" };
    rid_params_t pi = pg; pi.ignore = (const char (*)[21])ign; pi.n_ignore = 4;
    rid_table_update(&t, &pi, &OWN, 1000, &pk); CHECK(!pk.have);
    // stale: older than stale_s -> not eligible (blank)
    rid_table_update(&t, &pg, &OWN, 1000 + 5001, &pk); CHECK(!pk.have);
    // own GPS not ok -> nothing eligible
    rid_own_t bad = { false, 0, 0, false, 0 };
    rid_table_update(&t, &pg, &bad, 1000, &pk); CHECK(!pk.have);
    // most severe wins over nearest-but-capped; count
    rid_table_clear(&t); put_ac(&t, 3, 60, 1000, NULL, 2); put_ac(&t, 4, 200, 1000, "B", 2);
    put_ac(&t, 5, 900, 1000, "C", 2); put_ac(&t, 6, 1500, 1000, "D", 2);
    rid_table_update(&t, &P, &OWN, 1000, &pk);
    CHECK_EQ(pk.level, RID_LVL_WARN); CHECK(pk.dist_m >= 199 && pk.dist_m <= 201); CHECK_EQ(pk.count, 3);
    // expiry
    rid_table_update(&t, &P, &OWN, 1000 + RID_ENTRY_EXPIRE_MS + 1, &pk);
    int used = 0; for (int k = 0; k < RID_MAX_AC; k++) { used += t.ac[k].used; }
    CHECK_EQ(used, 0);
}

static void test_level_step(void)
{
    rid_lvl_state_t s = { 0 };
    rid_pick_t pk = { .have = true, .level = RID_LVL_ALERT, .hyst_level = RID_LVL_ALERT,
                      .alert_ok = true, .dist_m = 80 };
    // one own sample inside alert_m -> WARN only; the second -> ALERT
    CHECK_EQ(rid_level_step(&s, &pk, &P, 100, 100), RID_LVL_WARN);
    CHECK_EQ(rid_level_step(&s, &pk, &P, 100, 150), RID_LVL_WARN);     // same own sample: no count
    CHECK_EQ(rid_level_step(&s, &pk, &P, 600, 600), RID_LVL_ALERT);
    // one glitch sample outside: still ALERT (hysteresis), no flicker
    rid_pick_t out = pk; out.dist_m = 105; out.level = RID_LVL_WARN; out.hyst_level = RID_LVL_ALERT;
    CHECK_EQ(rid_level_step(&s, &out, &P, 1100, 1100), RID_LVL_ALERT);
    // beyond alert x1.15 (115 m) -> de-escalates only after 2 s
    out.dist_m = 120; out.hyst_level = RID_LVL_WARN;
    CHECK_EQ(rid_level_step(&s, &out, &P, 1600, 1600), RID_LVL_ALERT);
    CHECK_EQ(rid_level_step(&s, &out, &P, 2100, 3500), RID_LVL_ALERT);
    CHECK_EQ(rid_level_step(&s, &out, &P, 2600, 3700), RID_LVL_WARN);
    // re-escalation to ALERT needs two fresh samples again
    CHECK_EQ(rid_level_step(&s, &pk, &P, 3100, 3800), RID_LVL_WARN);
    CHECK_EQ(rid_level_step(&s, &pk, &P, 3600, 3900), RID_LVL_ALERT);
    // gone / stale -> clears at once
    rid_pick_t none = { 0 };
    CHECK_EQ(rid_level_step(&s, &none, &P, 4100, 4000), RID_LVL_NONE);
    // NOTICE -> NONE also holds 2 s
    rid_pick_t n = { .have = true, .level = RID_LVL_NOTICE, .hyst_level = RID_LVL_NOTICE, .alert_ok = true, .dist_m = 900 };
    CHECK_EQ(rid_level_step(&s, &n, &P, 4200, 5000), RID_LVL_NOTICE);
    n.level = RID_LVL_NONE; n.dist_m = 1100; n.hyst_level = RID_LVL_NOTICE;   // inside 1150
    CHECK_EQ(rid_level_step(&s, &n, &P, 4300, 9000), RID_LVL_NOTICE);
    n.dist_m = 1200; n.hyst_level = RID_LVL_NONE;
    CHECK_EQ(rid_level_step(&s, &n, &P, 4400, 9100), RID_LVL_NOTICE);
    CHECK_EQ(rid_level_step(&s, &n, &P, 4500, 11200), RID_LVL_NONE);
    // min +10 m on a small threshold
    rid_params_t p20 = { 1000, 300, 20, 5, 0, NULL, 0 };
    CHECK_EQ(rid_level_for(30, &p20, 115), RID_LVL_ALERT);
    CHECK_EQ(rid_level_for(31, &p20, 115), RID_LVL_WARN);
}

static void test_gate(void)
{
    CHECK(rid_gps_ok_eval(true, true, 3, 6, 6, 1000, 3999));
    CHECK(!rid_gps_ok_eval(true, true, 3, 6, 6, 1000, 4001));       // RAW_GPS older than 3 s
    CHECK(!rid_gps_ok_eval(true, true, 3, 5, 6, 1000, 1000));       // too few sats
    CHECK(!rid_gps_ok_eval(true, false, 3, 9, 6, 1000, 1000));      // no GPS sensor
    CHECK(!rid_gps_ok_eval(true, true, 0, 9, 6, 1000, 1000));       // no fix
    CHECK(!rid_gps_ok_eval(false, true, 3, 9, 6, 1000, 1000));      // FC link down
    CHECK(!rid_gps_ok_eval(true, true, 3, 9, 6, 0, 1000));          // never received
    rid_gate_t g = { 0 };
    CHECK(!rid_gate_step(&g, true, true, 1000));
    CHECK(!rid_gate_step(&g, true, true, 5999));
    CHECK(rid_gate_step(&g, true, true, 6000));                     // 5 s settle
    CHECK(rid_gate_step(&g, true, false, 7000));                    // dropout: grace
    CHECK(rid_gate_step(&g, true, true, 8000));                     // < 15 s: radios kept
    CHECK(rid_gate_step(&g, true, false, 9000));
    CHECK(rid_gate_step(&g, true, false, 23999));
    CHECK(!rid_gate_step(&g, true, false, 24000));                  // > 15 s: inactive
    CHECK(!rid_gate_step(&g, true, true, 25000));                   // settles again from scratch
    CHECK(rid_gate_step(&g, true, true, 30000));
    CHECK(!rid_gate_step(&g, false, true, 30001));                  // disabled: off at once
}

// ================================================================================================
static int render(char *out, uint8_t kind, uint8_t lvl, uint8_t n, uint32_t d, uint16_t brg,
                  bool hv, uint8_t style, bool imp, int *ai, uint8_t *gl)
{
    rid_render_in_t in = { kind, lvl, n, true, d, brg, hv, 0, style, imp, RID_VERT_UNKNOWN };
    rid_patch_t pt;
    int len = rid_render(out, 17, &in, &pt);
    *ai = pt.idx[0]; *gl = pt.glyph[0];
    CHECK_EQ(pt.idx[1], -1);                       // no vertical indicator without vert
    return len;
}

static void test_render(void)
{
    char o[17]; int ai; uint8_t gl;
    CHECK_EQ(render(o, RID_R_PROX, RID_LVL_NOTICE, 1, 820, 90, true, RID_BRG_AUTO, false, &ai, &gl), 12);
    CHECK(strcmp(o, "!RID1 820M ^") == 0); CHECK_EQ(ai, 11); CHECK_EQ(gl, 0x64);
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 240, 0, true, RID_BRG_AUTO, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 240M ^") == 0); CHECK_EQ(gl, 0x68);
    render(o, RID_R_PROX, RID_LVL_ALERT, 2, 80, 0, true, RID_BRG_AUTO, false, &ai, &gl);
    CHECK(strcmp(o, "!RID2 80M ^") == 0);
    // auto without a valid heading: compass text, never an arrow
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 240, 45, false, RID_BRG_AUTO, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 240M NE") == 0); CHECK_EQ(ai, -1);
    // glyph style without heading: blank bearing
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 240, 45, false, RID_BRG_GLYPH, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 240M") == 0); CHECK_EQ(ai, -1);
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 1240, 45, false, RID_BRG_DEGREES, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 1.2KM 045") == 0);
    // the id digit is always shown: 15 characters without the vertical indicator
    CHECK_EQ(render(o, RID_R_PROX, RID_LVL_ALERT, 9, 1240, 45, true, RID_BRG_DEGREES, false, &ai, &gl), 15);
    CHECK(strcmp(o, "!RID9 1.2KM 045") == 0);
    // imperial: feet below 1000 ft, then miles
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 300, 0, false, RID_BRG_COMPASS, true, &ai, &gl);
    CHECK(strcmp(o, "!RID1 984FT N") == 0);
    render(o, RID_R_PROX, RID_LVL_WARN, 1, 305, 0, false, RID_BRG_COMPASS, true, &ai, &gl);
    CHECK(strcmp(o, "!RID1 0.2MI N") == 0);
    render(o, RID_R_PROX, RID_LVL_NOTICE, 1, 1609, 0, false, RID_BRG_COMPASS, true, &ai, &gl);
    CHECK(strcmp(o, "!RID1 1.0MI N") == 0);
    render(o, RID_R_PROX, RID_LVL_NOTICE, 1, 999, 0, false, RID_BRG_COMPASS, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 999M N") == 0);
    render(o, RID_R_PROX, RID_LVL_NOTICE, 1, 1000, 0, false, RID_BRG_COMPASS, false, &ai, &gl);
    CHECK(strcmp(o, "!RID1 1.0KM N") == 0);
    // failure text
    CHECK_EQ(render(o, RID_R_FAIL, 0, 0, 0, 0, false, 0, false, &ai, &gl), 11);
    CHECK(strcmp(o, "RID RX FAIL") == 0); CHECK_EQ(ai, -1);
    // nothing for NONE / unknown kinds
    CHECK_EQ(render(o, RID_R_PROX, RID_LVL_NONE, 1, 10, 0, true, 0, false, &ai, &gl), 0);
    CHECK_EQ(render(o, RID_R_NONE, RID_LVL_ALERT, 1, 10, 0, true, 0, false, &ai, &gl), 0);

    // Exhaustive: every level x count x style x unit x heading over 0..20 km fits 16 and never
    // emits a byte outside 0x20..0x5F (the arrow is a placeholder, patched later).
    for (uint8_t lvl = 1; lvl <= 3; lvl++)
    for (uint8_t n = 0; n <= 9; n += 3)
    for (uint8_t st = 0; st <= 3; st++)
    for (int imp = 0; imp <= 1; imp++)
    for (int hv = 0; hv <= 1; hv++)
    for (uint32_t d = 0; d <= 20000; d += (d < 2000 ? 1 : 37)) {
        int len = render(o, RID_R_PROX, lvl, n, d, (uint16_t)(d % 360), hv, st, imp, &ai, &gl);
        CHECK(len <= 16); CHECK_EQ((int)strlen(o), len);
        for (int k = 0; k < len; k++) { if (o[k] < 0x20 || o[k] > 0x5F) { CHECK(0); } }
        if (ai >= 0) { CHECK(ai < len); CHECK_EQ(o[ai], RID_ARROW_PLACEHOLDER); CHECK(gl >= 0x60 && gl <= 0x6F); }
    }
    // small output buffers never overflow
    rid_render_in_t in = { RID_R_PROX, RID_LVL_ALERT, 9, true, 1240, 45, true, 0, RID_BRG_DEGREES, false, RID_VERT_UNKNOWN };
    for (size_t cap = 0; cap < 17; cap++) {
        char small[17]; memset(small, 'Z', sizeof(small));
        rid_patch_t pt;
        int len = rid_render(small, cap, &in, &pt);
        if (cap) { CHECK(len < (int)cap); CHECK_EQ(small[len], 0); }
    }
}

// ---- data minimisation: the entry and the JSON carry only identity + range/bearing -----------
static bool has(const char *s0, const char *k) { return strstr(s0, k) != NULL; }

static void test_minimal_entry_and_json(void)
{
    // The entry has no room for the dropped data: position (internal), status (internal), ID,
    // addresses, times, derived values and flags only.
    CHECK(sizeof(rid_ac_t) <= 80);   // + height (2), ref, vert, flag, dv (2) for above/below
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    enc_basic(m, "MIN-1", 1);
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1000);
    // a Location full of extra data (altitudes, speeds, direction, accuracies, timestamp)
    ODID_Location_data d; odid_initLocationData(&d);
    d.Status = 2; d.Latitude = 47.001; d.Longitude = 8.0; d.AltitudeBaro = 500; d.AltitudeGeo = 520;
    d.Height = 40; d.SpeedHorizontal = 12; d.SpeedVertical = 2; d.Direction = 90;
    d.HorizAccuracy = 10; d.TimeStamp = 100; d.TSAccuracy = 3;
    ODID_Location_encoded e; encodeLocationMessage(&e, &d); memcpy(m, &e, 25);
    CHECK_EQ(rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1100), i);
    rid_params_t p = { 1000, 300, 100, 5, 0, NULL, 0 };
    rid_pick_t pk; rid_table_update(&t, &p, &OWN, 1200, &pk);
    CHECK(pk.have); CHECK_EQ(pk.level, RID_LVL_WARN);         // geometry/levels need nothing dropped

    char j[RID_AC_JSON_MAX];
    int n = rid_ac_json(j, sizeof j, &t.ac[i], 1300, true, 0);
    CHECK(n > 0); CHECK_EQ((int)strlen(j), n);
    CHECK(has(j, "\"id\":\"MIN-1\"")); CHECK(has(j, "\"src\":\"ble\"")); CHECK(has(j, "\"age_ms\":200"));
    CHECK(has(j, "\"ignored\":false")); CHECK(has(j, "\"level\":\"warn\""));
    CHECK(has(j, "\"dist_m\":111")); CHECK(has(j, "\"brg\":0"));
    static const char *const k_never[] = { "lat", "lon", "alt", "height", "speed", "heading", "acc",
        "\"ts", "rssi", "type", "status", "msgs", "first", "47.", "8.0" };
    for (size_t k = 0; k < sizeof k_never / sizeof k_never[0]; k++) { if (has(j, k_never[k])) { CHECK(0); } }
    // test-card form: no ignored / level
    n = rid_ac_json(j, sizeof j, &t.ac[i], 1300, false, 0);
    CHECK(!has(j, "ignored")); CHECK(!has(j, "level")); CHECK(has(j, "dist_m"));
    // no own fix (the reception test indoors): listed, no range / bearing
    rid_own_t none = { false, 0, 0, false, 0 };
    rid_table_update(&t, &p, &none, 1300, &pk);
    n = rid_ac_json(j, sizeof j, &t.ac[i], 1300, false, 0);
    CHECK(!has(j, "dist_m")); CHECK(!has(j, "brg")); CHECK(has(j, "MIN-1"));
    // pseudo-ID before a Basic ID; HEX for binary IDs; escaping; worst-case length fits
    rid_table_clear(&t);
    enc_loc(m, 47.001, 8.0, 0, 2);
    i = rid_table_ingest_msg(&t, RID_TR_WIFI, W1, m, &OWN, 10);
    rid_ac_json(j, sizeof j, &t.ac[i], 10, true, 0);
    CHECK(has(j, "\"id\":\"WIFI:F6:E5:D4:C3:B2:A1\"")); CHECK(has(j, "\"src\":\"wifi\""));
    rid_ac_t w; memset(&w, 0, sizeof w);
    w.used = w.id_valid = true; memset(w.uas_id, 0xAB, 20); w.src_mask = 3;
    w.dist_valid = true; w.dist_m = 19999; w.brg_deg = 359; w.eligible = true; w.level = 3;
    n = rid_ac_json(j, sizeof j, &w, 0xFFFFFFFFu, true, 0);
    CHECK(has(j, "HEXABABABABABABABABABABABABABABABABABABABAB") || has(j, "HEX:ABABABABABABABABABABABABABABABABABABABAB"));
    CHECK(n < RID_AC_JSON_MAX);
    w.id_printable = true; strcpy(w.uas_id, "A\"B\\C");
    rid_ac_json(j, sizeof j, &w, 0, true, 0); CHECK(has(j, "\"id\":\"A\\\"B\\\\C\""));
    CHECK_EQ(rid_ac_json(j, 10, &w, 0, true, 0), 0);             // too small a buffer: nothing
}

// ---- above / below (owner decision 2026-11-18) -------------------------------------------------
static void test_vert_step(void)
{
    CHECK_EQ(rid_vert_step(RID_VERT_UNKNOWN, false, 100), RID_VERT_UNKNOWN);   // unknown -> nothing
    CHECK_EQ(rid_vert_step(RID_VERT_UNKNOWN, true, 16), RID_VERT_ABOVE);       // first: plain band
    CHECK_EQ(rid_vert_step(RID_VERT_UNKNOWN, true, 15), RID_VERT_LEVEL);
    CHECK_EQ(rid_vert_step(RID_VERT_UNKNOWN, true, -16), RID_VERT_BELOW);
    // LEVEL -> ABOVE needs > 18 (band + 3); ABOVE holds down to 12 (band - 3)
    CHECK_EQ(rid_vert_step(RID_VERT_LEVEL, true, 17), RID_VERT_LEVEL);
    CHECK_EQ(rid_vert_step(RID_VERT_LEVEL, true, 19), RID_VERT_ABOVE);
    CHECK_EQ(rid_vert_step(RID_VERT_ABOVE, true, 12), RID_VERT_ABOVE);
    CHECK_EQ(rid_vert_step(RID_VERT_ABOVE, true, 11), RID_VERT_LEVEL);
    CHECK_EQ(rid_vert_step(RID_VERT_ABOVE, true, -40), RID_VERT_BELOW);
    CHECK_EQ(rid_vert_step(RID_VERT_BELOW, true, -12), RID_VERT_BELOW);
    CHECK_EQ(rid_vert_step(RID_VERT_BELOW, true, -11), RID_VERT_LEVEL);
    CHECK_EQ(rid_vert_step(RID_VERT_LEVEL, true, -19), RID_VERT_BELOW);
    CHECK_EQ(rid_vert_step(RID_VERT_ABOVE, false, 50), RID_VERT_UNKNOWN);      // own alt lost
}

static void test_vert_table(void)
{
    rid_table_t t; rid_table_clear(&t);
    uint8_t m[25];
    ODID_Location_data d; odid_initLocationData(&d);
    d.Status = 2; d.Latitude = 47.001; d.Longitude = 8.0; d.Height = 80; d.HeightType = 0;
    d.AltitudeGeo = 999; d.AltitudeBaro = 999;            // must NOT be used
    ODID_Location_encoded e; encodeLocationMessage(&e, &d); memcpy(m, &e, 25);
    int i = rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1000);
    CHECK(t.ac[i].height_valid); CHECK_EQ(t.ac[i].height_m, 80);
    rid_params_t p = { 1000, 300, 100, 5, 0, NULL, 0 };
    rid_pick_t pk;
    rid_own_t own = OWN; own.alt_ok = true; own.alt_cm = 5000;   // we are 50 m above our take-off
    rid_table_update(&t, &p, &own, 1100, &pk);
    CHECK_EQ(pk.vert, RID_VERT_ABOVE); CHECK_EQ(pk.dv_m, 30);
    char j[RID_AC_JSON_MAX]; rid_ac_json(j, sizeof j, &t.ac[i], 1100, true, 0);
    CHECK(strstr(j, "\"vert\":\"above\",\"dv_m\":30") != NULL);
    CHECK(strstr(j, "height") == NULL && strstr(j, "alt") == NULL);   // raw heights never exposed
    own.alt_cm = 6800; rid_table_update(&t, &p, &own, 1200, &pk); CHECK_EQ(pk.vert, RID_VERT_ABOVE);  // dv 12: held
    own.alt_cm = 7900; rid_table_update(&t, &p, &own, 1300, &pk); CHECK_EQ(pk.vert, RID_VERT_LEVEL);
    own.alt_cm = 11000; rid_table_update(&t, &p, &own, 1400, &pk); CHECK_EQ(pk.vert, RID_VERT_BELOW); CHECK_EQ(pk.dv_m, -30);
    own.alt_ok = false; rid_table_update(&t, &p, &own, 1500, &pk); CHECK_EQ(pk.vert, RID_VERT_UNKNOWN);
    rid_ac_json(j, sizeof j, &t.ac[i], 1500, true, 0); CHECK(strstr(j, "vert") == NULL);
    // other height unknown (INV_ALT) -> unknown
    d.Height = INV_ALT; encodeLocationMessage(&e, &d); memcpy(m, &e, 25);
    rid_table_ingest_msg(&t, RID_TR_BLE, A1, m, &OWN, 1600);
    own.alt_ok = true; rid_table_update(&t, &p, &own, 1700, &pk);
    CHECK(!t.ac[i].height_valid); CHECK_EQ(pk.vert, RID_VERT_UNKNOWN);
}

static void test_vert_render_budget(void)
{
    char o[17]; rid_patch_t pt;
    rid_render_in_t in = { RID_R_PROX, RID_LVL_ALERT, 9, true, 1240, 45, true, 0, RID_BRG_DEGREES, false, RID_VERT_ABOVE };
    // worst case is exactly 16 and fits with the id digit: '+' / '-' are plain text, never patched
    CHECK_EQ(rid_render(o, sizeof o, &in, &pt), 16); CHECK(strcmp(o, "!RID9 1.2KM 045+") == 0);
    CHECK_EQ(pt.idx[0], -1); CHECK_EQ(pt.idx[1], -1);
    in.vert = RID_VERT_LEVEL;  rid_render(o, sizeof o, &in, &pt); CHECK(strcmp(o, "!RID9 1.2KM 045") == 0);
    in.vert = RID_VERT_BELOW; in.bearing = RID_BRG_COMPASS;
    rid_render(o, sizeof o, &in, &pt); CHECK(strcmp(o, "!RID9 1.2KM NE-") == 0);
    in.bearing = RID_BRG_GLYPH; in.vert = RID_VERT_ABOVE;
    CHECK_EQ(rid_render(o, sizeof o, &in, &pt), 14); CHECK(strcmp(o, "!RID9 1.2KM ^+") == 0);
    CHECK_EQ(pt.idx[0], 12); CHECK_EQ(pt.idx[1], -1);
    in.hdg_valid = false;                                  // glyph style, no heading: blank bearing
    rid_render(o, sizeof o, &in, &pt); CHECK(strcmp(o, "!RID9 1.2KM +") == 0);
    CHECK_EQ(pt.idx[0], -1);
    in.vert = RID_VERT_BELOW; rid_render(o, sizeof o, &in, &pt); CHECK(strcmp(o, "!RID9 1.2KM -") == 0);
    in.bang_on = false; rid_render(o, sizeof o, &in, &pt); CHECK(strcmp(o, " RID9 1.2KM -") == 0);  // only the '!' blinks
    // exhaustive: every style x units x vert x heading x count over 0..20 km fits 16, nothing
    // outside 0x20..0x5F, patches inside the text and pointing at placeholders
    for (uint8_t st = 0; st <= 3; st++) for (int imp = 0; imp <= 1; imp++)
    for (uint8_t v = 0; v <= 3; v++) for (int hv = 0; hv <= 1; hv++)
    for (uint8_t n = 0; n <= 8; n += 4) for (uint8_t lvl = 1; lvl <= 3; lvl++)
    for (uint32_t d = 0; d <= 20000; d += (d < 1100 ? 3 : 97)) {
        rid_render_in_t q = { RID_R_PROX, lvl, n, true, d, (uint16_t)(d % 360), hv, 0, st, imp, v };
        int len = rid_render(o, sizeof o, &q, &pt);
        CHECK(len <= 16);
        for (int k = 0; k < len; k++) { if (o[k] < 0x20 || o[k] > 0x5F) { CHECK(0); } }
        for (int k = 0; k < 2; k++) if (pt.idx[k] >= 0) { CHECK(pt.idx[k] < len); CHECK_EQ(o[pt.idx[k]], RID_ARROW_PLACEHOLDER); }
        bool want_v = (v == RID_VERT_ABOVE || v == RID_VERT_BELOW);
        char last = len ? o[len - 1] : 0;
        bool glyph_style = st == RID_BRG_GLYPH || (st == RID_BRG_AUTO && hv);
        if (want_v && last != (v == RID_VERT_ABOVE ? '+' : '-')) { CHECK(0); }
        if (!want_v && (last == '+' || last == '-')) { CHECK(0); }
        if (pt.idx[1] != -1) { CHECK(0); }
        if (!glyph_style && pt.idx[0] != -1) { CHECK(0); }
    }
}

// ---- one message at a time, ALERT priority, blinking "!" ----------------------------------------
static rid_msg_t M(uint8_t lvl, uint8_t id) { rid_msg_t m; memset(&m, 0, sizeof m); m.level = lvl; m.id = id; return m; }

static void test_seq_and_bang(void)
{
    rid_seq_t q;
    rid_seq_select(NULL, 0, 0, &q); CHECK(!q.replace);
    // WARN / NOTICE: normal line first, then each message 2 s, then the cycle repeats
    rid_msg_t w[2] = { M(RID_LVL_WARN, 1), M(RID_LVL_NOTICE, 2) };
    rid_seq_select(w, 2, 0, &q);    CHECK(!q.replace);
    rid_seq_select(w, 2, 1999, &q); CHECK(!q.replace);
    rid_seq_select(w, 2, 2000, &q); CHECK(q.replace); CHECK(!q.alert); CHECK_EQ(q.idx, 0); CHECK_EQ(q.level, RID_LVL_WARN);
    rid_seq_select(w, 2, 4000, &q); CHECK(q.replace); CHECK_EQ(q.idx, 1); CHECK_EQ(q.level, RID_LVL_NOTICE);
    rid_seq_select(w, 2, 6000, &q); CHECK(!q.replace);                  // cycle = 2 + 2*2 s
    // ALERT priority: only alerts, one after the other, never the normal line or WARN
    rid_msg_t a[3] = { M(RID_LVL_ALERT, 3), M(RID_LVL_ALERT, 4), M(RID_LVL_WARN, 1) };
    for (uint32_t t = 0; t < 20000; t += 500) {
        rid_seq_select(a, 3, t, &q);
        CHECK(q.replace); CHECK(q.alert); CHECK_EQ(q.idx, (t / 2000) % 2); CHECK_EQ(q.level, RID_LVL_ALERT);
    }
    // blinking: NOTICE steady, WARN 1 Hz, ALERT 2 Hz; steady when too slow to show or disabled
    for (uint32_t t = 0; t < 2000; t += 50) { CHECK(rid_bang_on(RID_LVL_NOTICE, t, 10, true)); }
    CHECK(rid_bang_on(RID_LVL_WARN, 0, 2, true));    CHECK(!rid_bang_on(RID_LVL_WARN, 500, 2, true));
    CHECK(rid_bang_on(RID_LVL_WARN, 1000, 2, true)); CHECK(rid_bang_on(RID_LVL_WARN, 500, 1, true));
    CHECK(rid_bang_on(RID_LVL_ALERT, 0, 4, true));   CHECK(!rid_bang_on(RID_LVL_ALERT, 250, 4, true));
    CHECK(rid_bang_on(RID_LVL_ALERT, 250, 3, true)); CHECK(rid_bang_on(RID_LVL_ALERT, 250, 10, false));
}

int main(void)
{
    RUN(test_ble_match);
    RUN(test_ble_match_fuzz);
    RUN(test_pack_split);
    RUN(test_wifi_match);
    RUN(test_wifi_match_fuzz);
    RUN(test_table_basic);
    RUN(test_table_uasid_unterminated);
    RUN(test_table_range_reject);
    RUN(test_table_merge_dual);
    RUN(test_table_eviction);
    RUN(test_jump_reject);
    RUN(test_geo);
    RUN(test_arrow);
    RUN(test_levels_and_caps);
    RUN(test_level_step);
    RUN(test_gate);
    RUN(test_render);
    RUN(test_minimal_entry_and_json);
    RUN(test_vert_step);
    RUN(test_vert_table);
    RUN(test_vert_render_budget);
    RUN(test_seq_and_bang);
    TEST_MAIN_END();
}
