// cfg.h -- versioned configuration blob in NVS (spec 11).
//
// Persistence contract (spec 11.0):
//  * cfg_t lives in NVS namespace "cfg", key "blob", CRC32-checked.
//  * NimBLE bond keys live in their own namespace ("nimble_bond"), stack-managed.
//  * user-initiated writes (/api/config, pair, forget) commit SYNCHRONOUSLY.
//  * version mismatch -> migrate, NEVER wipe. Corrupt -> defaults + FLAG_CFG_RESET
//    + erase orphan bond.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CFG_MAGIC   0x43424247u   // "CBBG"
#define CFG_VERSION 4u

// Max simultaneously-bonded cameras (spec 7.1). One connected at a time; the
// rest are candidates the reconnect scan picks from by RSSI.
#define CAM_MAX_BONDS 4

// Per-camera BLE telemetry poll interval, in DECISECONDS (100 ms units). The
// driver re-pulls camera status (recording / battery / SD / ...) this often
// while connected. Portal offers three presets: fast 0.5 s / normal 1 s /
// slow 1.5 s. Faster congests the BLE link + notify queue; slower makes the
// OSD stale.
#define CAM_POLL_DS_MIN     5u    // 0.5 s ("fast")
#define CAM_POLL_DS_MAX     15u   // 1.5 s ("slow")
#define CAM_POLL_DS_DEFAULT 10u   // 1.0 s ("normal")

#define CFG_DEFAULT_WIFI_PW "reclinkfpv"   // AP password on factory defaults

// -------- status flags (spec 3.6). Runtime, surfaced in /api/status.flags -----
typedef enum {
    FLAG_CFG_RESET        = 1u << 0,
    FLAG_REPAIR_NEEDED    = 1u << 1,
    FLAG_OSD_UNSUPPORTED  = 1u << 2,
    FLAG_FC_OFFLINE       = 1u << 3,
    FLAG_CLOCK_SYNC_FAIL  = 1u << 4,
    FLAG_WDT_RECOVERED    = 1u << 5,
} bridge_flag_t;

// Runtime flag store (flags.c). Not persisted -- recomputed each boot.
void     bridge_flag_set(bridge_flag_t f);
void     bridge_flag_clear(bridge_flag_t f);
bool     bridge_flag_get(bridge_flag_t f);
uint32_t bridge_flags_all(void);

// -------- record modes -------------------------------------------------------
// REC_ARM_AND_SWITCH: despite the name (kept for on-flash compatibility), this
// is OR-with-override, not a strict AND -- records while armed OR while the
// switch is on; flipping the switch OFF while armed cuts the clip and it stays
// cut (even though still armed) until the switch goes back ON (spec 8.1 combi
// "cut & restart"). REC_STRICT_AND is the literal AND: recording tracks
// `armed && switch` live, no latch -- both conditions true at once, no more.
enum { REC_OFF = 0, REC_ARM = 1, REC_SWITCH = 2, REC_ARM_AND_SWITCH = 3, REC_STRICT_AND = 4 };
// record.sw.mode: how the assigned AUX channel drives the record trigger.
//   SWITCH -- a real 2-position switch. Above threshold = recording, below =
//             not, mirrored live (spec 8.2 hysteresis). Release stops the clip.
//   BUTTON -- a momentary / spring-loaded control. Each press (crossing the
//             threshold) TOGGLES the desired state once; letting go does
//             nothing, so a non-latching control can still start AND stop.
enum { RECSW_SWITCH = 0, RECSW_BUTTON = 1 };
// -------- portal trigger ----------------------------------------------------
enum { PORTAL_TRIG_BUTTON = 0, PORTAL_TRIG_ALWAYS_DISARMED = 1 };
// -------- RF transmit power presets (wifi_tx_power / ble_tx_power) ----------
// Applied to esp_wifi_set_max_tx_power() / esp_ble_tx_power_set() -- see
// portal_task.c / ble_central.c for the actual dBm mapping per radio. Default
// is MAX on both, matching the prior (implicit, unconfigurable) behaviour.
enum { RF_PWR_LOW = 0, RF_PWR_MEDIUM = 1, RF_PWR_MAX = 2 };

// One stored camera bond.
typedef struct __attribute__((packed)) {
    char     brand[6];       // "gopro" | "dji"; "" = empty slot
    uint8_t  addr[6];
    char     name[32];
    uint32_t dji_device_id;
} cam_bond_t;

// ---------------------------------------------------------------------------
// The blob. Layout is frozen per CFG_VERSION; changes require a bump + migration.
// __attribute__((packed)) so the on-flash image is portable across compilers.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    // The *active* camera: the one currently connected, or the one the reconnect
    // scan should prefer. Mirrors one entry of cams[] (below). `bonded` == "an
    // active camera is set". Kept at this offset so v1 blobs stay CRC-valid.
    struct {
        char     brand[6];        // "gopro" | "dji" | ""
        uint8_t  bonded;
        uint8_t  addr[6];
        char     name[32];
        uint32_t dji_device_id;   // persistent DUML controller id (active cam)
    } camera;

    struct {
        uint8_t  mode;            // REC_*
        uint16_t arm_delay_ms;
        uint16_t stop_delay_ms;
        uint16_t verify_timeout_ms;
        uint8_t  retry_count;
        struct {
            uint8_t  channel;     // AUX 1..14 (0 = none)
            uint16_t threshold_us;
            uint8_t  mode;        // RECSW_* -- was the low byte of a removed
                                   // debounce_ms (2026-08-30); same 2-byte size,
                                   // so old blobs load as mode=0 (switch).
            uint8_t  _rsvd;
        } sw;
    } record;

    struct {
        uint8_t  enabled;
        uint8_t  channel;
        uint16_t dwell_ms;
        uint8_t  allow_during_record;
        struct { uint16_t max_us; uint32_t id; } bands[3];
    } preset;

    struct {
        uint8_t  refresh_hz;
        uint8_t  type;            // 0 analog (full charset) / 1 digital (DJI-goggle-safe subset)
        uint8_t  corner;          // 0 TL 1 TR 2 BL 3 BR
        char     line[4][24];
        uint32_t fields_mask;
        struct {
            uint8_t batt_pct;
            uint8_t sd_low_min;
            uint8_t blink;
            uint8_t show_on_lost;
        } warn;
        uint8_t  units;           // 0 metric 1 imperial
    } osd;

    struct {
        uint8_t  enabled;
        uint8_t  require_gps;
        uint8_t  min_sats;
        uint16_t threshold_s;
        int16_t  utc_offset_min;
        uint8_t  dst;
        uint8_t  check_period_min;
        uint16_t resync_period_min;
        uint8_t  retry_count;
    } clock;

    struct {
        uint8_t  enabled;
        uint8_t  rate_hz;
    } gps_push;

    struct {
        uint8_t  channel;
    } hilight;

    // was `interlock` -- removed (passenger-only, spec 1.2.1). One byte reclaimed
    // for msp.swap_pins; layout/offsets unchanged so old blobs stay CRC-valid.
    uint8_t  msp_swap_pins;      // 1 = feed TX_GPIO as RX and RX_GPIO as TX
    uint8_t  ui_flags;           // portal prefs: bit0 advanced mode, bit1 suppress default-password nag

    struct {
        uint32_t baud;
    } msp;

    struct {
        char     ssid[20];
        char     password[32];
        uint8_t  channel;
        uint8_t  portal_trigger;  // PORTAL_TRIG_*
        uint8_t  portal_timeout_min;
    } wifi;

    uint8_t  led_brightness;
    uint32_t crc32;              // over [magic .. led_brightness]

    // ---- v2+ tail: appended AFTER crc32, so v1 blobs still validate and just
    // migrate. Not covered by crc32 -- a corrupt entry only costs a failed
    // connect attempt, and the real bond keys live in the nimble_bond store.
    cam_bond_t cams[CAM_MAX_BONDS];

    // ---- v3+ tail: per-camera BLE poll interval (deciseconds), indexed like
    // cams[]. 0 == use default. Appended at the very end; cams[] layout is
    // unchanged so v2 blobs migrate without re-spacing.
    uint8_t  cam_poll_ds[CAM_MAX_BONDS];

    // ---- v4+ tail: RF transmit power presets (RF_PWR_*), one per radio.
    uint8_t  wifi_tx_power;      // portal AP -- applied every time it comes up
    uint8_t  ble_tx_power;       // camera link -- applied at boot; reboot to change
} cfg_t;

// -------- OSD token bit positions for cfg.osd.fields_mask --------------------
enum {
    OSD_TOK_ST = 0, OSD_TOK_ET, OSD_TOK_RT, OSD_TOK_BT, OSD_TOK_BB,
    OSD_TOK_RS, OSD_TOK_FP, OSD_TOK_LN, OSD_TOK_SD, OSD_TOK_MD,
    OSD_TOK_CN, OSD_TOK_WR,
    OSD_TOK_AR, OSD_TOK_PR, OSD_TOK_BD, OSD_TOK_CR, OSD_TOK_GS, OSD_TOK_GF,
    OSD_TOK_FB, OSD_TOK_SP, OSD_TOK_SG,
    OSD_TOK_BI,
    OSD_TOK_COUNT
};

// ---------------------------------------------------------------------------
// API. cfg_init() must run first in app_main (spec 3.6 step 1).
// ---------------------------------------------------------------------------

// Load + validate + migrate. Sets *reset_flag if defaults were substituted.
// Also reports whether the caller must erase the orphan NimBLE bond.
esp_err_t cfg_init(bool *out_cfg_reset, bool *out_erase_orphan_bond);

// Pointer to the live, in-RAM last-good blob. Read-only for callers other than
// the portal. Never NULL after cfg_init().
const cfg_t *cfg_get(void);

// Copy the live blob for editing.
void cfg_snapshot(cfg_t *dst);

// Validate + clamp a candidate blob in place. Returns ESP_ERR_INVALID_ARG with
// a human message in err_buf if a field is unrecoverably wrong.
esp_err_t cfg_validate(cfg_t *cand, char *err_buf, size_t err_len);

// Synchronous commit (spec 11.0): validate -> recompute size/crc -> nvs_set_blob
// -> nvs_commit -> swap into RAM. Returns only after the write is durable.
esp_err_t cfg_commit(const cfg_t *cand);

// Camera bond helpers -- each is a synchronous commit.
//
// Add or update a bond and make it the active camera. If addr already known,
// its entry is refreshed. Returns ESP_ERR_NO_MEM when all CAM_MAX_BONDS slots
// are full and addr is new.
esp_err_t cfg_set_camera_bond(const char *brand, const uint8_t addr[6],
                              const char *name, uint32_t dji_device_id);
// Forget one bond by address; packs the list. If it was the active camera, the
// active slot moves to cams[0] (or clears when the list is now empty).
esp_err_t cfg_forget_camera(const uint8_t addr[6]);
esp_err_t cfg_clear_camera(void);          // forget ALL bonds + clear active
// Make an already-bonded camera the active one (reconnect picked it). No-op if
// addr is not in the list. Cheap commit.
esp_err_t cfg_set_active_camera(const uint8_t addr[6]);

// Set the BLE telemetry poll interval (deciseconds) for a bonded camera. Clamped
// to [CAM_POLL_DS_MIN, CAM_POLL_DS_MAX]. ESP_ERR_NOT_FOUND if addr is not bonded.
esp_err_t cfg_cam_set_poll(const uint8_t addr[6], uint8_t poll_ds);
// Poll interval (ms) for the *active* camera, always clamped/defaulted. For drivers.
uint16_t  cfg_active_poll_ms(void);

// Bond list access. Copies slot i into *out (and *poll_ds if non-NULL); returns
// false if oob/empty.
int  cfg_cam_count(void);                         // number of non-empty slots
bool cfg_cam_at(int i, cam_bond_t *out, uint8_t *poll_ds);

// Factory reset: erase cfg + caller erases nimble_bond, then reboot.
esp_err_t cfg_factory_erase(void);

void cfg_load_defaults(cfg_t *dst);

// -------- deferred runtime counters: namespace "stats" (spec 11.1) ----------
typedef struct {
    uint32_t reconnects;
    uint32_t msp_crc_errors;
    uint32_t clock_sync_fail;
} cfg_stats_t;

void cfg_stats_load(cfg_stats_t *dst);
void cfg_stats_bump_reconnect(void);
void cfg_stats_bump_msp_crc(void);
void cfg_stats_flush(void);                 // housekeeping, <= 1 / 30 s
