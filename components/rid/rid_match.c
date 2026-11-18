// rid_match.c -- the two radio-callback matchers + pack splitting (spec 8.8.2.2 / 8.8.3 / 8.8.4).
//
// These run inside vendor task context (NimBLE host, Wi-Fi driver task), outside the rid_task
// isolation: a bug here panics the whole bridge (spec 8.8.8). So they are small, allocation-free,
// log-free, bounded by the length they are given, and fuzzed on the host (test_rid.c).
#include "rid_core.h"

#include <string.h>

// ---- BLE legacy advert ------------------------------------------------------------------------
// AD structure: [len][0x16 service data, 16-bit UUID][0xFA 0xFF = UUID 0xFFFA][0x0D app code]
// [counter][25-byte message] -> len >= 30. Verified against sxjack id_scanner @ 627a328 and the
// OpenDroneID Android receiver @ 4cfaaef (VERIFY.md RID-BLE-ADV). The walk visits at most
// len/2 structures of a <= 31-byte payload (bounded; O(1) in practice).
bool rid_ble_match(const uint8_t *adv, size_t len, const uint8_t **msg)
{
    if (!adv || !msg) { return false; }
    size_t off = 0;
    while (off + 1 < len) {
        size_t sl = adv[off];                         // structure length (type + data)
        if (sl == 0) { break; }                       // early terminator
        if (off + 1 + sl > len) { break; }            // lying length: stop, never read past len
        const uint8_t *s = adv + off + 1;             // s[0] = AD type
        if (sl >= 1 + 2 + 1 + 1 + RID_MSG_SIZE &&
            s[0] == 0x16 && s[1] == 0xFA && s[2] == 0xFF && s[3] == 0x0D) {
            *msg = s + 5;                             // after type, UUID(2), app code, counter
            return true;
        }
        off += 1 + sl;
    }
    return false;
}

// ---- pack header ------------------------------------------------------------------------------
// [0xF << 4 | version][single message size = 25][count 1..9] then count x 25 bytes
// (opendroneid.h:599-610, wifi.c:539-559; VERIFY.md RID-PACK).
static int pack_count(const uint8_t *p, size_t len)
{
    if (len < RID_PACK_HDR) { return -1; }
    if ((p[0] >> 4) != 0xF) { return -1; }
    if (p[1] != RID_MSG_SIZE) { return -1; }
    int n = p[2];
    if (n < 1 || n > RID_PACK_MAX_MSGS) { return -1; }
    if ((size_t)RID_PACK_HDR + (size_t)n * RID_MSG_SIZE > len) { return -1; }
    return n;
}

int rid_pack_split(const uint8_t *pack, size_t len, const uint8_t *msgs[RID_PACK_MAX_MSGS])
{
    if (!pack || !msgs) { return -1; }
    int n = pack_count(pack, len);
    if (n < 0) { return -1; }
    for (int i = 0; i < n; i++) { msgs[i] = pack + RID_PACK_HDR + (size_t)i * RID_MSG_SIZE; }
    return n;
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static const uint8_t k_asd_oui[3]   = { 0xFA, 0x0B, 0xBC };                    // wifi.c:464
static const uint8_t k_nan_da[6]    = { 0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00 };  // wifi.c:569
static const uint8_t k_wfa_oui[3]   = { 0x50, 0x6F, 0x9A };
static const uint8_t k_nan_svc[6]   = { 0x88, 0x69, 0x19, 0x9D, 0x92, 0x09 };  // wifi.c:571

#define MGMT_HDR      24       // FC(2) dur(2) DA(6) SA(6) BSSID(6) seq(2)
#define BEACON_FIXED  12       // timestamp(8) interval(2) capability(2)

// A pack whose header is sane and whose size is exactly what the header says (core-c requires
// `ret == pack_len` for NAN, wifi.c:630-634). `avail` bounds it.
static bool pack_exact(const uint8_t *p, size_t avail, size_t *out_len)
{
    int n = pack_count(p, avail);
    if (n < 0) { return false; }
    *out_len = (size_t)RID_PACK_HDR + (size_t)n * RID_MSG_SIZE;
    return true;
}

static int match_beacon(const uint8_t *f, size_t len, uint8_t src[6],
                        const uint8_t **pack, size_t *pack_len)
{
    // core-c has no beacon RECEIVE function: the layout is its builder (wifi.c:457-537), its
    // unit test and the Android receiver. IE order is not fixed, so walk the IE list
    // (VERIFY.md RID-WIFI-RXMATCH). Bounded by len: each step advances >= 2 bytes.
    size_t off = MGMT_HDR + BEACON_FIXED;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        size_t  il = f[off + 1];
        if (off + 2 + il > len) { break; }                       // truncated / lying IE length
        const uint8_t *ie = f + off + 2;
        // DD len FA 0B BC 0D <counter> <pack>
        if (id == 0xDD && il >= 3 + 1 + 1 + RID_PACK_HDR &&
            memcmp(ie, k_asd_oui, 3) == 0 && ie[3] == 0x0D) {
            size_t plen;
            if (pack_exact(ie + 5, il - 5, &plen)) {
                memcpy(src, f + 10, 6);                           // SA
                *pack = ie + 5;
                *pack_len = plen;
                return RID_WIFI_KIND_BEACON;
            }
            return RID_WIFI_KIND_NONE;
        }
        off += 2 + il;
    }
    return RID_WIFI_KIND_NONE;
}

static int match_nan(const uint8_t *f, size_t len, uint8_t src[6],
                     const uint8_t **pack, size_t *pack_len)
{
    // odid_wifi_receive_message_pack_nan_action_frame (core-c wifi.c:561-651) field by field.
    // The trailing Service Descriptor Extension attribute is NOT required (a leaner matcher,
    // VERIFY.md RID-WIFI-RXMATCH "design choice"): it carries nothing we use.
    if (len < MGMT_HDR + 6 + 13 + 1) { return RID_WIFI_KIND_NONE; }
    if (memcmp(f + 4, k_nan_da, 6) != 0) { return RID_WIFI_KIND_NONE; }
    const uint8_t *b = f + MGMT_HDR;
    if (b[0] != 0x04 || b[1] != 0x09) { return RID_WIFI_KIND_NONE; }         // public action, vendor
    if (memcmp(b + 2, k_wfa_oui, 3) != 0 || b[5] != 0x13) { return RID_WIFI_KIND_NONE; }
    const uint8_t *a = b + 6;                                                 // SDA
    if (a[0] != 0x03) { return RID_WIFI_KIND_NONE; }
    uint16_t alen = (uint16_t)(a[1] | (a[2] << 8));
    if (memcmp(a + 3, k_nan_svc, 6) != 0) { return RID_WIFI_KIND_NONE; }
    if (a[9] != 0x01 || a[11] != 0x10) { return RID_WIFI_KIND_NONE; }       // instance 1, follow-up
    size_t sil = a[12];                                                       // service info length
    if (sil < 1 + RID_PACK_HDR) { return RID_WIFI_KIND_NONE; }
    if (alen != 10 + sil) { return RID_WIFI_KIND_NONE; }
    size_t si_off = MGMT_HDR + 6 + 13;
    if (si_off + sil > len) { return RID_WIFI_KIND_NONE; }
    const uint8_t *p = f + si_off + 1;                                        // after the message counter
    size_t plen;
    if (!pack_exact(p, sil - 1, &plen) || plen != sil - 1) { return RID_WIFI_KIND_NONE; }
    memcpy(src, f + 10, 6);
    *pack = p;
    *pack_len = plen;
    return RID_WIFI_KIND_NAN;
}

int rid_wifi_match(const uint8_t *frame, size_t len, uint8_t src[6],
                   const uint8_t **pack, size_t *pack_len)
{
    if (!frame || !src || !pack || !pack_len || len < MGMT_HDR) { return RID_WIFI_KIND_NONE; }
    uint8_t fc0 = frame[0];
    if ((fc0 & 0x0C) != 0x00) { return RID_WIFI_KIND_NONE; }        // type must be management
    uint8_t stype = (uint8_t)(fc0 & 0xF0);
    if (stype == 0x80) { return match_beacon(frame, len, src, pack, pack_len); }
    if (stype == 0xD0) { return match_nan(frame, len, src, pack, pack_len); }
    return RID_WIFI_KIND_NONE;
}
