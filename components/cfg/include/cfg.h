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
#define CFG_VERSION 1u

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
enum { REC_OFF = 0, REC_ARM = 1, REC_SWITCH = 2, REC_ARM_AND_SWITCH = 3 };
// -------- portal trigger ----------------------------------------------------
enum { PORTAL_TRIG_BUTTON = 0, PORTAL_TRIG_ALWAYS_DISARMED = 1 };
// -------- interlock requirement -------------------------------------------
enum { INTERLOCK_CONNECTED = 0, INTERLOCK_RECORDING = 1 };

// ---------------------------------------------------------------------------
// The blob. Layout is frozen per CFG_VERSION; changes require a bump + migration.
// __attribute__((packed)) so the on-flash image is portable across compilers.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    struct {
        char     brand[6];        // "gopro" | "dji" | ""
        uint8_t  bonded;
        uint8_t  addr[6];
        char     name[32];
        uint32_t dji_device_id;   // persistent DUML controller id
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
            uint16_t debounce_ms;
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
        uint8_t  auto_place;
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

    struct {
        uint8_t  enabled;
        uint8_t  require;         // INTERLOCK_*
    } interlock;

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
} cfg_t;

// -------- OSD token bit positions for cfg.osd.fields_mask --------------------
enum {
    OSD_TOK_ST = 0, OSD_TOK_ET, OSD_TOK_RT, OSD_TOK_BT, OSD_TOK_BB,
    OSD_TOK_RS, OSD_TOK_FP, OSD_TOK_LN, OSD_TOK_SD, OSD_TOK_MD,
    OSD_TOK_CN, OSD_TOK_WR, OSD_TOK_COUNT
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

// Camera metadata helpers -- each is a synchronous commit.
esp_err_t cfg_set_camera_bond(const char *brand, const uint8_t addr[6],
                              const char *name, uint32_t dji_device_id);
esp_err_t cfg_clear_camera(void);          // forget: zero camera.* + commit

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
