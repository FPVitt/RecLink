// rid_core.h -- Remote ID receiver (F7, spec 8.8): the PURE logic. No FreeRTOS, no ESP-IDF,
// no logging, no allocation -- so the host tests (test/host/test_rid.c) compile it as is.
//
// Everything received over the air is UNTRUSTED (spec 13.11): every function here is bounded by
// the length it is given, never trusts a length field inside the data, and keeps received data in
// the caller's RAM only (spec 13.10). Nothing derived from it can reach a path that influences
// the aircraft -- its only outputs are OSD text and the bench portal's JSON.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- constants (spec 8.8.4 / 8.8.5 / 8.8.8) ----------------------------------------------
#define RID_MAX_AC              8         // static aircraft table entries
#define RID_MAX_TRACK_M         20000u    // beyond this an aircraft is not tracked (geometry limit)
#define RID_ID_GRACE_MS         15000u    // un-identified entries capped at NOTICE this long
#define RID_GPS_SETTLE_MS       5000u     // rid_gps_ok must hold this long before "active"
#define RID_GPS_LOST_GRACE_MS   15000u    // !rid_gps_ok this long before "inactive"
#define RID_GPS_MAX_AGE_MS      3000u     // last MSP_RAW_GPS reply at most this old
#define RID_WIFI_START_DELAY_MS 2000u     // RID_RX not before this after the first decoded arm
#define RID_WIFI_STOP_HOLD_MS   3000u     // decoded disarm held this long before RID_RX stops
#define RID_DEESC_HOLD_MS       2000u     // de-escalation: beyond threshold x1.15 this long
#define RID_ENTRY_EXPIRE_MS     60000u    // an entry not heard for this long is removed
#define RID_MAX_SPEED_MPS       255u      // ODID speed maximum: a bigger implied jump is rejected
#define RID_JUMP_SLACK_M        20u       // GPS error allowance on the jump check (both ends)
#define RID_ACCEPT_PER_S        64u       // per-transport accept cap in the radio callbacks
// Vertical relation (owner decision 2026-11-18): other aircraft's ODID HEIGHT vs our height above
// the arming point (MSP_ALTITUDE). Not geodetic (WGS-84 ellipsoid vs Betaflight's MSL: tens of m)
// and not baro (pressure altitude). Right on a shared flat field, approximate otherwise.
#define RID_LEVEL_BAND_M        15        // |dv| <= band: LEVEL
#define RID_VERT_HYST_M         3         // the band must be crossed by this much to switch
#define RID_OWN_ALT_MAX_AGE_MS  1000u     // own MSP_ALTITUDE older than this: unknown

#define RID_MSG_SIZE            25        // ODID_MESSAGE_SIZE
#define RID_ID_SIZE             20        // ODID_ID_SIZE
#define RID_PACK_HDR            3         // [0xF<<4|ver][msg size = 25][count 1..9]
#define RID_PACK_MAX_MSGS       9         // ODID_PACK_MAX_MESSAGES
#define RID_PACK_BYTES_MAX      (RID_PACK_HDR + RID_MSG_SIZE * RID_PACK_MAX_MSGS)   // 228

enum { RID_TR_BLE = 0, RID_TR_WIFI = 1, RID_TR_N = 2 };
typedef enum { RID_LVL_NONE = 0, RID_LVL_NOTICE = 1, RID_LVL_WARN = 2, RID_LVL_ALERT = 3 } rid_level_t;
enum { RID_WIFI_KIND_NONE = 0, RID_WIFI_KIND_BEACON = 1, RID_WIFI_KIND_NAN = 2 };
enum { RID_BRG_AUTO = 0, RID_BRG_GLYPH = 1, RID_BRG_COMPASS = 2, RID_BRG_DEGREES = 3 };
enum { RID_VERT_UNKNOWN = 0, RID_VERT_ABOVE = 1, RID_VERT_BELOW = 2, RID_VERT_LEVEL = 3 };
// ODID_STATUS_* (opendroneid.h:154-158, VERIFY.md RID-CORE-STATUS)
enum { RID_STATUS_UNDECLARED = 0, RID_STATUS_GROUND = 1, RID_STATUS_AIRBORNE = 2,
       RID_STATUS_EMERGENCY = 3, RID_STATUS_RID_FAILURE = 4 };

// ---- callback matchers (run in the NimBLE host task / the Wi-Fi task) ------------------------
// Both are allocation-free, log-free, and bounded by `len`. They only recognise the frame and
// point into it; the caller copies.

// BLE legacy advertising data (the AD structures, <= 31 bytes): finds an AD structure
// `len 0x16 FA FF 0D <counter> <25-byte message>` (spec 8.8.3, VERIFY.md RID-BLE-ADV).
// Returns true and *msg -> the 25 message bytes on a match.
bool rid_ble_match(const uint8_t *adv, size_t len, const uint8_t **msg);

// One 802.11 management frame, `len` = usable bytes (rx_ctrl.sig_len - 4 FCS). Recognises a
// Beacon with the ASD-STAN vendor IE (FA:0B:BC type 0x0D) or a NAN Service Discovery frame to
// 51:6F:9A:01:00:00 with the ODID service id (spec 8.8.2.2, VERIFY.md RID-WIFI-SIG / -RXMATCH).
// Returns RID_WIFI_KIND_*; on a match *pack/-len point at the message pack (header included,
// length = exactly 3 + 25 x count, <= 228) and src[] holds the transmitter address.
int rid_wifi_match(const uint8_t *frame, size_t len, uint8_t src[6],
                   const uint8_t **pack, size_t *pack_len);

// Split a message pack (bounds-checked against len; never trusts the count beyond what fits).
// Returns the number of messages (1..9) with msgs[i] -> 25 bytes each, or -1 if malformed.
int rid_pack_split(const uint8_t *pack, size_t len, const uint8_t *msgs[RID_PACK_MAX_MSGS]);

// ---- geometry (spec 8.8.5) -----------------------------------------------------------------
// |lat| <= 90e7, |lon| <= 180e7 and not the 0/0 "unknown" value.
bool rid_latlon_ok(int32_t lat_1e7, int32_t lon_1e7);
// Local-tangent-plane distance + bearing from own (0) to target (1). Deltas in int64, the
// 20 km gate applied before any trigonometry. false = beyond RID_MAX_TRACK_M (or bad input).
bool rid_geo(int32_t lat0, int32_t lon0, int32_t lat1, int32_t lon1,
             uint32_t *dist_m, uint16_t *brg_deg);

// ---- heading / arrow (spec 8.8.5, Betaflight osd_elements.c:591) ----------------------------
#define RID_SYM_ARROW_SOUTH 0x60
#define RID_SYM_ARROW_SMALL_UP   0x75   // Betaflight osd_symbols.h:98 @ 2025.12.5
#define RID_SYM_ARROW_SMALL_DOWN 0x76   // :99
// ABOVE / BELOW / LEVEL with hysteresis from the previous relation (UNKNOWN = none yet).
uint8_t rid_vert_step(uint8_t prev, bool have, int32_t dv_m);
uint8_t     rid_arrow_glyph(uint16_t brg_deg, int16_t hdg_deg);   // 0x60..0x6F
const char *rid_compass8(uint16_t brg_deg);                        // "N" "NE" ... "NW"

// ---- the aircraft table (spec 8.8.4) --------------------------------------------------------
// Data minimisation (owner decision 2026-11-18): an entry holds ONLY what identifies an aircraft
// and computes / displays its range and bearing. Everything else a Basic ID or Location message
// carries (UA type, altitudes, height, speeds, direction, accuracies, timestamp) is discarded at
// decode time; Operator ID / Self-ID / System / Auth are discarded unparsed; RSSI is not kept.
typedef struct {                     // ordered widest-first: several copies are allocated
    int32_t  lat_1e7, lon_1e7;       // INTERNAL ONLY: distance / bearing / jump check; never exposed
    uint32_t first_seen_ms;          // the 15 s ID grace (internal)
    uint32_t last_loc_ms;            // stale rule + jump check (internal)
    uint32_t last_any_ms;            // age (exposed as age_ms)
    uint32_t dist_m;                 // derived each pass (rid_table_update)
    uint16_t brg_deg;                // derived
    int16_t  height_m;               // ODID height (the ONE kept altitude field), whole metres
    int16_t  dv_m;                   // derived: other height - own height (valid with vert)
    uint8_t  addr[RID_TR_N][6];      // entry key / pseudo-ID before a Basic ID: BLE address, Wi-Fi MAC
    char     uas_id[RID_ID_SIZE + 1];// forced NUL-terminated
    uint8_t  src_mask;               // bit0 heard on BLE, bit1 on Wi-Fi
    uint8_t  id_type;                // only to rank two IDs and to render text vs HEX:
    uint8_t  status;                 // INTERNAL ONLY: the "ground" eligibility rule
    uint8_t  level;                  // derived: capped level for this entry
    uint8_t  height_ref;             // 0 above take-off, 1 above ground (internal)
    uint8_t  vert;                   // derived: RID_VERT_* (also the hysteresis memory)
    uint8_t  hyst_level;             // derived: level with thresholds x1.15 (min +10 m), capped
    bool     alert_ok;               // derived: its caps allow ALERT
    bool     height_valid;
    bool     used;
    bool     id_valid;
    bool     id_printable;           // serial / CAA IDs only (others may be binary)
    bool     pos_valid;
    bool     dist_valid;             // derived
    bool     eligible;               // derived
    bool     ignored;                // derived
} rid_ac_t;

typedef struct {
    rid_ac_t ac[RID_MAX_AC];
    uint32_t jump_rejects, range_rejects, evict_drops;   // counters (no data)
} rid_table_t;

// What the pure logic needs from cfg.rid (kept separate so the tests need no cfg_t).
typedef struct {
    uint16_t notice_m, warn_m, alert_m;
    uint8_t  stale_s;
    uint8_t  ground;                 // 0: "ground" aircraft never exceed NOTICE
    const char (*ignore)[RID_ID_SIZE + 1];
    uint8_t  n_ignore;
} rid_params_t;

typedef struct {                     // own position, only when the GPS gate is good
    bool    ok;
    int32_t lat_1e7, lon_1e7;
    bool    alt_ok;                  // own height above the arming point known (armed, <= 1 s old)
    int32_t alt_cm;
} rid_own_t;

void rid_table_clear(rid_table_t *t);
// One 25-byte message. Basic ID and Location are decoded (opendroneid-core-c per-message
// decoders) and reduced at once to the identity / position fields above; every other type --
// Operator ID, Self-ID, System, Auth -- is discarded UNPARSED.
// Returns the entry index touched, or -1.
int  rid_table_ingest_msg(rid_table_t *t, uint8_t transport, const uint8_t addr[6],
                          const uint8_t msg[RID_MSG_SIZE], const rid_own_t *own, uint32_t now);
// A Wi-Fi message pack: split + ingest each message (Basic ID first so the ID keys the rest).
// Returns the number of messages ingested, -1 if the pack is malformed.
int  rid_table_ingest_pack(rid_table_t *t, const uint8_t addr[6],
                           const uint8_t *pack, size_t len, const rid_own_t *own, uint32_t now);

// The result of one pass over the table: the aircraft the OSD should talk about.
typedef struct {
    bool     have;                   // an eligible aircraft (the most severe, then the nearest)
    uint8_t  level;                  // its capped level (before hysteresis / two-sample rule)
    uint8_t  hyst_level;             // the level with thresholds x1.15 (min +10 m), capped
    bool     alert_ok;               // its caps allow ALERT
    uint32_t dist_m;
    uint16_t brg_deg;
    uint8_t  count;                  // eligible aircraft inside notice_m (merged by ID)
    uint8_t  vert;                   // RID_VERT_* of the picked aircraft
    int16_t  dv_m;
    bool     near_have;              // nearest eligible aircraft (any distance) -- portal
    uint32_t near_dist_m;
} rid_pick_t;

// Recompute dist/brg/eligible/level for every entry, expire old entries, and pick. With
// own->ok false nothing is eligible (outputs blank -- spec 8.8.5 grace).
void rid_table_update(rid_table_t *t, const rid_params_t *p, const rid_own_t *own,
                      uint32_t now, rid_pick_t *out);
// Is this UAS ID on the ignore list?
bool rid_id_ignored(const rid_params_t *p, const char *id);

// The per-aircraft JSON object for the portal (pure, shared by GET /api/rid and GET /api/rid/test,
// host-tested): {"id","src","age_ms"} [+ "n" = the number shown as RIDx on the OSD] [+ "ignored" and, when eligible, "level"] [+ "dist_m","brg"
// only with a computed distance] [+ "vert","dv_m" only with a known vertical relation]. Nothing else is ever emitted -- no position, no RSSI, no
// message contents. Returns strlen(out) (0 if cap is too small; cap >= RID_AC_JSON_MAX is safe).
#define RID_AC_JSON_MAX 192
int rid_ac_json(char *out, size_t cap, const rid_ac_t *a, uint32_t now, bool table_fields,
                uint8_t num);   // num = the aircraft's OSD number 1..9 (its table slot + 1; 0 = omit)


// Raw level for a distance (no caps). scale_pct = 100 normally, 115 for the hysteresis test
// (with a minimum of +10 m on each threshold).
uint8_t rid_level_for(uint32_t dist_m, const rid_params_t *p, int scale_pct);

// ---- displayed level: escalation, two-sample ALERT, hysteresis (spec 8.8.5) ---------------
typedef struct {
    uint8_t  cur;                    // displayed level
    uint8_t  alert_streak;           // consecutive own-position samples inside alert_m
    uint32_t below_since;            // hysteretic level below cur since (0 = not)
    uint32_t last_own_sample;        // gps_rx_ms of the last own sample counted
} rid_lvl_state_t;
// `own_sample_ms` = fc.gps_rx_ms (a change = a new own-position sample).
uint8_t rid_level_step(rid_lvl_state_t *s, const rid_pick_t *pk, const rid_params_t *p,
                       uint32_t own_sample_ms, uint32_t now);
// The same step for ONE table entry (each aircraft has its own displayed level: the OSD talks
// about one aircraft at a time). An ineligible / unused entry clears its state.
uint8_t rid_entry_level_step(rid_lvl_state_t *s, const rid_ac_t *a, const rid_params_t *p,
                             uint32_t own_sample_ms, uint32_t now);

// ---- the OSD message sequence (owner decision 2026-11-18) -----------------------------------
// One message per aircraft, shown ONE AT A TIME. rid_task publishes the displayed aircraft sorted
// level-descending then nearest-first. ALERT messages have priority and are the ONLY ones shown
// while any exists (cycled one after the other, the whole %rp line taken over, no normal line);
// otherwise the WARN / NOTICE messages are cycled with the normal OSD line in between
// (normal line, msg 1, msg 2, ... , normal line, ...).
#define RID_MSG_MS      2000u        // each message is on screen this long
#define RID_NORMAL_MS   2000u        // the normal line between rounds of WARN / NOTICE messages
typedef struct {                     // one displayed aircraft (8 B); nothing else about it is kept
    uint16_t dist_m;
    uint16_t brg_deg;
    uint8_t  level;                  // RID_LVL_NOTICE..ALERT (displayed level)
    uint8_t  vert;                   // RID_VERT_*
    uint8_t  id;                     // the aircraft's number 1..9 shown as "RIDx" (its table slot + 1:
                                     // stable while the aircraft is tracked; the portal list shows it too)
    uint8_t  _pad;
} rid_msg_t;
typedef struct {
    bool    replace;                 // true: the %rp line shows msgs[idx]; false: the normal line
    bool    alert;                   // the shown message is an ALERT
    uint8_t idx;
    uint8_t level;
} rid_seq_t;
void rid_seq_select(const rid_msg_t *msgs, uint8_t n, uint32_t now, rid_seq_t *out);
// The "!" at the start of every message: steady at NOTICE, slow blink (1 Hz) at WARN, fast blink
// (2 Hz) at ALERT. Only the "!" blinks. Steady when blinking is off or the refresh is too slow to
// show it (WARN needs >= 2 Hz, ALERT >= 4 Hz).
bool rid_bang_on(uint8_t level, uint32_t now, uint8_t refresh_hz, bool blink_enabled);

// ---- activation gate (spec 8.8.5) ----------------------------------------------------------
typedef struct {
    bool     active;
    uint32_t ok_since, bad_since;    // 0 = not running
} rid_gate_t;
bool rid_gps_ok_eval(bool link_up, bool gps_present, uint8_t fix, uint8_t sats, uint8_t min_sats,
                     uint32_t gps_rx_ms, uint32_t now);
bool rid_gate_step(rid_gate_t *g, bool enabled, bool gps_ok, uint32_t now);

// ---- the %rp text (spec 8.8.7) -------------------------------------------------------------
//   !RIDx <distance> <bearing arrow><+|->     x = the aircraft's id 1..9 (stable while tracked)
enum { RID_R_NONE = 0, RID_R_PROX = 1, RID_R_FAIL = 2 };
#define RID_ARROW_PLACEHOLDER '^'    // inside 0x20..0x5F: survives the digital filter, patched after
typedef struct {
    uint8_t  kind;                   // RID_R_*
    uint8_t  level;                  // RID_LVL_* (PROX)
    uint8_t  index;                  // the aircraft's id 1..9 (0 = none shown)
    bool     bang_on;                // the leading "!" is visible (it is the only thing that blinks)
    uint32_t dist_m;
    uint16_t brg_deg;
    bool     hdg_valid;
    int16_t  hdg_deg;
    uint8_t  bearing;                // RID_BRG_*
    bool     imperial;
    uint8_t  vert;                   // RID_VERT_*: ABOVE '+' / BELOW '-' after the bearing, always text
} rid_render_in_t;
// Glyph patches: [0] = the bearing arrow ([1] is unused since the vertical indicator became
// '+' / '-'). idx -1 = none.
typedef struct { int8_t idx[2]; uint8_t glyph[2]; } rid_patch_t;
// Renders into out (<= 16 chars + NUL, cap >= 17). The bearing arrow is emitted as
// RID_ARROW_PLACEHOLDER and listed in *pt, to be patched in AFTER the digital filter and the
// 16-char clamp. Worst case is exactly 16 (`!RID1 1.2KM 045+`). Returns strlen(out).
int rid_render(char *out, size_t cap, const rid_render_in_t *in, rid_patch_t *pt);
