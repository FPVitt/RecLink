#include "cfg.h"
#include "crc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define NS_CFG   "cfg"
#define KEY_BLOB "blob"

bool cfg_migrate_chain(cfg_t *c);   // cfg_migrate.c

// Readers (any task) always see a COMPLETE config: cfg_get() returns an immutable snapshot, and a
// commit builds the next one in a spare slot and publishes it with one pointer store. Three
// rotating slots keep the previous snapshot intact for two further commits, so a reader that is
// part-way through a read when a commit lands (commits are rare and disarmed-only) never sees a
// half-written struct. Callers must therefore re-fetch cfg_get() instead of holding the pointer
// for the life of a task.
static cfg_t s_slot[3];
static cfg_t *volatile s_cur = &s_slot[0];
#define s_live (*s_cur)                 // last-good, always valid after cfg_init

// Serialises writers (the portal and the camera task both commit). Created in cfg_init.
static SemaphoreHandle_t s_wlock;
static StaticSemaphore_t s_wlock_buf;
static void wlock(void)   { if (s_wlock) { xSemaphoreTake(s_wlock, portMAX_DELAY); } }
static void wunlock(void) { if (s_wlock) { xSemaphoreGive(s_wlock); } }
static bool  s_ready;

// CRC covers everything from `magic` up to but not including `crc32`.
static uint32_t blob_crc(const cfg_t *c)
{
    return crc32_iso_hdlc((const uint8_t *)c, offsetof(cfg_t, crc32));
}

const cfg_t *cfg_get(void)
{
    return s_ready ? s_cur : NULL;
}

void cfg_snapshot(cfg_t *dst)
{
    *dst = s_live;
}

// --------------------------------------------------------------------------
// Validation / clamping. Anything out of range is clamped to a safe value and
// logged; only structurally impossible input returns an error.
// --------------------------------------------------------------------------
#define CLAMP(x, lo, hi) do { if ((x) < (lo)) (x) = (lo); else if ((x) > (hi)) (x) = (hi); } while (0)

esp_err_t cfg_validate(cfg_t *c, char *err, size_t errlen)
{
    if (err && errlen) { err[0] = '\0'; }

    if (c->camera.brand[0] &&
        strcmp(c->camera.brand, "gopro") != 0 &&
        strcmp(c->camera.brand, "dji") != 0) {
        if (err) { snprintf(err, errlen, "camera.brand must be gopro|dji"); }
        return ESP_ERR_INVALID_ARG;
    }

    CLAMP(c->record.mode, REC_OFF, REC_STRICT_AND);
    CLAMP(c->record.arm_delay_ms, 0, 10000);
    CLAMP(c->record.stop_delay_ms, 0, 20000);
    CLAMP(c->record.verify_timeout_ms, 100, 5000);
    CLAMP(c->record.retry_count, 0, 10);
    CLAMP(c->record.sw.channel, 0, 14);
    CLAMP(c->record.sw.threshold_us, 900, 2100);
    CLAMP(c->record.sw.mode, RECSW_SWITCH, RECSW_BUTTON);
    c->record.sw.flags = 0;                      // reserved (see cfg.h)

    CLAMP(c->preset.channel, 0, 14);
    CLAMP(c->preset.dwell_ms, 50, 5000);
    // Zones: 2..PRESET_ZONES_MAX. Every edge but the last zone's is editable and the
    // edges must ascend with >= 25 us between them (leaving room for the zones above);
    // the last zone's top is pinned at 2100. Zones beyond the count are unused: cleared,
    // so one added later starts empty.
    int nz = cfg_zones(c);
    c->n_zones = (uint8_t)nz;
    for (int k = 0; k < nz - 1; k++) {
        int lo = k == 0 ? 925 : cfg_zone_max_us(c, k - 1) + 25;
        int hi = 2075 - 25 * (nz - 2 - k);
        int v = cfg_zone_max_us(c, k);
        if (v < lo) { v = lo; } else if (v > hi) { v = hi; }
        cfg_zone_set(c, k, (uint16_t)v, cfg_zone_id(c, k));
    }
    cfg_zone_set(c, nz - 1, 2100, cfg_zone_id(c, nz - 1));
    for (int k = nz; k < PRESET_ZONES_MAX; k++) {
        cfg_zone_set(c, k, 0, 0);
        memset(cfg_zone_gp_mut(c, k), 0, sizeof(cfg_gp_zone_t));
    }

    // GoPro zone settings: keep only whitelisted ids, no duplicates, in the
    // canonical apply order (gp_preset.h). Anything else is silently dropped.
    for (int z = 0; z < nz; z++) {
        cfg_gp_zone_t *g = cfg_zone_gp_mut(c, z);
        if (cfg_gp_zone_is_preset(g)) { memset(g->s, 0, sizeof(g->s)); continue; }   // simple zone: marker only
        uint8_t val[GP_PSET_MAX]; bool has[GP_PSET_MAX] = { false };
        for (int k = 0; k < g->n && k < GP_PSET_MAX; k++) {
            int idx = gp_pset_index(g->s[k].id);
            if (idx >= 0) { has[idx] = true; val[idx] = g->s[k].val; }
        }
        memset(g, 0, sizeof(*g));
        for (int idx = 0; idx < GP_PSET_MAX; idx++) {
            if (!has[idx]) { continue; }
            g->s[g->n].id  = gp_pset_id(idx);
            g->s[g->n].val = val[idx];
            g->n++;
        }
    }

    CLAMP(c->hilight.channel, 0, 14);
    cfg_osd_retire_tokens(c);       // tokens removed from the product are dropped from saved lines

    CLAMP(c->osd.refresh_hz, 1, 10);
    CLAMP(c->osd.corner, 0, 3);
    CLAMP(c->osd.type, 0, 1);
    // New OSD tokens appended over time: if a blob has every classic token on
    // but none of the newer ones, turn the newer ones on too -- up to OSD_TOK_BI only.
    // OSD_TOK_RP (Remote ID) and anything after it is set once by its migration; OR-ing it
    // back in here on every validate would make %rp impossible to switch off (spec 8.8.9).
    if ((c->osd.fields_mask & 0xFFFu) == 0xFFFu) {
        c->osd.fields_mask |= ((1u << OSD_TOK_RP) - 1u);
    }
    // The Protune group (OSD_TOK_IX..OSD_TOK_PT) arrived after %rp: a blob that has every older
    // token on (bits below OSD_TOK_IX, %rp included) and none of the group is an upgrade, so
    // turn the group on once. Any one of them on means the blob already knows about them.
    {
        const uint32_t older = (1u << OSD_TOK_IX) - 1u;
        if ((c->osd.fields_mask & older) == older && !(c->osd.fields_mask & OSD_TOK_PROTUNE_MASK)) {
            c->osd.fields_mask |= OSD_TOK_PROTUNE_MASK;
        }
    }
    for (int i = 0; i < 4; i++) { c->osd.line[i][sizeof(c->osd.line[i]) - 1] = '\0'; }
    CLAMP(c->osd.warn.batt_pct, 0, 100);
    CLAMP(c->osd.warn.sd_low_min, 0, 60);
    CLAMP(c->osd.units, 0, 1);
    CLAMP(c->led_brightness, 16, 255);   // 0 would blank every status indication

    CLAMP(c->clock.min_sats, 3, 32);
    if (c->clock.threshold_s < 2) { c->clock.threshold_s = 2; }
    CLAMP(c->clock.threshold_s, 2, 3600);
    CLAMP(c->clock.utc_offset_min, -840, 840);
    CLAMP(c->clock.check_period_min, 1, 240);
    if (c->clock.resync_period_min < 1) { c->clock.resync_period_min = 1; }
    CLAMP(c->clock.retry_count, 0, 10);

    CLAMP(c->gps_push.rate_hz, 1, 2);

    CLAMP(c->msp_swap_pins, 0, 1);
    CLAMP(c->wifi_tx_power, RF_PWR_LOW, RF_PWR_MAX);
    CLAMP(c->ble_tx_power, RF_PWR_LOW, RF_PWR_MAX);
    c->ui_flags &= 0x03;

    if (c->msp.baud != 9600 && c->msp.baud != 19200 && c->msp.baud != 38400 &&
        c->msp.baud != 57600 && c->msp.baud != 115200 && c->msp.baud != 230400 &&
        c->msp.baud != 250000 && c->msp.baud != 460800) {
        c->msp.baud = 115200;
    }

    c->wifi.ssid[sizeof(c->wifi.ssid) - 1] = '\0';
    c->wifi.password[sizeof(c->wifi.password) - 1] = '\0';
    if (strlen(c->wifi.password) < 8) {
        if (err) { snprintf(err, errlen, "wifi.password must be >= 8 chars"); }
        return ESP_ERR_INVALID_ARG;
    }
    CLAMP(c->wifi.portal_trigger, PORTAL_TRIG_BUTTON, PORTAL_TRIG_ALWAYS_DISARMED);
    CLAMP(c->wifi.portal_timeout_min, 1, 120);
    c->camera.brand[sizeof(c->camera.brand) - 1] = '\0';
    c->camera.name[sizeof(c->camera.name) - 1] = '\0';

    // Bond list: sanitise silently (recoverable data, not CRC-protected). Clear
    // any slot whose brand is not exactly gopro|dji, then compact cams[] and the
    // parallel cam_poll_ds[] together.
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        cam_bond_t *b = &c->cams[i];
        b->brand[sizeof(b->brand) - 1] = '\0';
        b->name[sizeof(b->name) - 1]   = '\0';
        if (b->brand[0] &&
            strcmp(b->brand, "gopro") != 0 && strcmp(b->brand, "dji") != 0) {
            memset(b, 0, sizeof(*b));
            c->cam_poll_ds[i] = 0;
        }
        if (b->brand[0]) {
            uint8_t w = c->cam_poll_ds[i] & CAM_POLL_WAKE, p = c->cam_poll_ds[i] & (uint8_t)~CAM_POLL_WAKE;
            if (p == 0) { p = CAM_POLL_DS_DEFAULT; }
            CLAMP(p, CAM_POLL_DS_MIN, CAM_POLL_DS_MAX);
            c->cam_poll_ds[i] = (uint8_t)(p | w);
        }
    }
    for (int i = 0, seen = 0; i < CAM_MAX_BONDS; i++) {      // wake at power-up: one camera at most
        if (c->cam_poll_ds[i] & CAM_POLL_WAKE) { if (seen++) { c->cam_poll_ds[i] &= (uint8_t)~CAM_POLL_WAKE; } }
    }
    for (int i = 0, w = 0; i < CAM_MAX_BONDS; i++) {
        if (c->cams[i].brand[0]) {
            if (w != i) {
                c->cams[w] = c->cams[i];       memset(&c->cams[i], 0, sizeof(c->cams[i]));
                c->cam_poll_ds[w] = c->cam_poll_ds[i]; c->cam_poll_ds[i] = 0;
            }
            w++;
        }
    }
    // Keep camera.bonded consistent with whether an active brand is set.
    c->camera.bonded = c->camera.brand[0] ? 1 : 0;

    cfg_rid_validate(c);              // Remote ID tail (spec 8.8.9): clamp / re-order, never reject

    return ESP_OK;
}

// --------------------------------------------------------------------------
static esp_err_t write_blob(const cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_blob(h, KEY_BLOB, c, sizeof(*c));
    if (err == ESP_OK) { err = nvs_commit(h); }   // transactional (spec 12.5)
    nvs_close(h);
    return err;
}

static void publish_locked(const cfg_t *src)
{
    cfg_t *next = &s_slot[((int)(s_cur - s_slot) + 1) % 3];
    *next = *src;
    __sync_synchronize();               // the whole struct is written before the pointer moves
    s_cur = next;
}

static int bond_index_of(const cfg_t *c, const uint8_t addr[6]);

// ---- camera nicknames: one blob under "nicks", cached in RAM (read lock-free, written under the writer lock)
#define KEY_NICKS "nicks"
typedef struct __attribute__((packed)) { uint8_t addr[6]; char nick[CAM_NICK_LEN]; } cam_nick_t;
static cam_nick_t s_nick[CAM_MAX_BONDS];

static esp_err_t nick_store(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_blob(h, KEY_NICKS, s_nick, sizeof(s_nick));
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    return err;
}

static void nick_load(void)
{
    memset(s_nick, 0, sizeof(s_nick));
    nvs_handle_t h;
    if (nvs_open(NS_CFG, NVS_READONLY, &h) != ESP_OK) { return; }
    size_t sz = sizeof(s_nick);
    cam_nick_t tmp[CAM_MAX_BONDS];
    if (nvs_get_blob(h, KEY_NICKS, tmp, &sz) == ESP_OK && sz == sizeof(tmp)) {
        for (int i = 0; i < CAM_MAX_BONDS; i++) {
            tmp[i].nick[CAM_NICK_LEN - 1] = '\0';
            if (bond_index_of(&s_live, tmp[i].addr) >= 0) { s_nick[i] = tmp[i]; }   // drop orphans
            else { memset(&s_nick[i], 0, sizeof(s_nick[i])); }
        }
    }
    nvs_close(h);
}

static int nick_slot(const uint8_t a[6])
{
    for (int i = 0; i < CAM_MAX_BONDS; i++) { if (s_nick[i].nick[0] && !memcmp(s_nick[i].addr, a, 6)) { return i; } }
    return -1;
}

static void nick_clear_locked(const uint8_t a[6])              // RAM only; the caller stores
{
    int i = nick_slot(a);
    if (i >= 0) { memset(&s_nick[i], 0, sizeof(s_nick[i])); }
}

bool cfg_cam_nick_get(const uint8_t addr[6], char out[CAM_NICK_LEN])
{
    int i = nick_slot(addr);
    if (i < 0) { return false; }
    memcpy(out, s_nick[i].nick, CAM_NICK_LEN);
    out[CAM_NICK_LEN - 1] = '\0';
    return true;
}

esp_err_t cfg_cam_nick_set(const uint8_t addr[6], const char *nick)
{
    wlock();
    esp_err_t e = ESP_OK;
    if (bond_index_of(&s_live, addr) < 0) {
        e = ESP_ERR_NOT_FOUND;
    } else {
        char clean[CAM_NICK_LEN]; int n = 0;
        for (const char *p = nick ? nick : ""; *p && n < CAM_NICK_LEN - 1; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch >= 0x20 && ch <= 0x7e && !(n == 0 && ch == ' ')) { clean[n++] = (char)ch; }   // printable, no leading blanks
        }
        while (n > 0 && clean[n - 1] == ' ') { n--; }
        clean[n] = '\0';
        nick_clear_locked(addr);
        if (n > 0) {
            int slot = -1;
            for (int i = 0; i < CAM_MAX_BONDS; i++) { if (!s_nick[i].nick[0]) { slot = i; break; } }
            if (slot < 0) { e = ESP_ERR_NO_MEM; }
            else { memcpy(s_nick[slot].addr, addr, 6); memcpy(s_nick[slot].nick, clean, sizeof(clean)); }
        }
        if (e == ESP_OK) { e = nick_store(); }
    }
    wunlock();
    return e;
}

// ---- per-camera settings storage (cfg_profile_t, see cfg.h) -------------------------------
// One NVS blob per bonded camera, key "p<12 hex digits of the address>" (13 chars, under the
// 15-char NVS key limit). Written BEFORE the main blob on a camera switch: a crash in between
// leaves the main blob still naming the outgoing camera with its own (identical) settings.
static void prof_key(char key[16], const uint8_t a[6])
{
    snprintf(key, 16, "p%02x%02x%02x%02x%02x%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

static esp_err_t prof_store(const uint8_t a[6], const cfg_profile_t *p)
{
    char key[16]; prof_key(key, a);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_blob(h, key, p, sizeof(*p));
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "camera settings write failed: %s", esp_err_to_name(err)); }
    return err;
}

static bool prof_load(const uint8_t a[6], cfg_profile_t *p)
{
    char key[16]; prof_key(key, a);
    nvs_handle_t h;
    if (nvs_open(NS_CFG, NVS_READONLY, &h) != ESP_OK) { return false; }
    size_t sz = sizeof(*p);
    memset(p, 0, sizeof(*p));
    bool ok = nvs_get_blob(h, key, p, &sz) == ESP_OK && sz == sizeof(*p) && cfg_profile_valid(p);
    nvs_close(h);
    return ok;
}

static void prof_erase(const uint8_t a[6])
{
    char key[16]; prof_key(key, a);
    nvs_handle_t h;
    if (nvs_open(NS_CFG, NVS_READWRITE, &h) != ESP_OK) { return; }
    nvs_erase_key(h, key);                       // ESP_ERR_NVS_NOT_FOUND is fine
    nvs_commit(h);
    nvs_close(h);
}

// Camera switch: save the outgoing camera's settings (the live head fields) under its own key, and
// load the incoming camera's into the working copy c. A camera with no stored profile (or a
// freshly paired one) keeps what c already has, i.e. inherits. Called with the writer lock held,
// before the main blob is committed.
static esp_err_t switch_settings_locked(cfg_t *c, bool had_old, const uint8_t old_addr[6],
                                        const uint8_t new_addr[6], bool fresh)
{
    if (had_old) {
        cfg_profile_t out; cfg_profile_extract(&s_live, &out);
        esp_err_t e = prof_store(old_addr, &out);
        if (e != ESP_OK) { return e; }
    }
    if (!fresh) {
        cfg_profile_t in;
        if (prof_load(new_addr, &in) && cfg_profile_apply(c, &in)) {
            ESP_LOGI(TAG, "camera settings loaded for %02x:%02x:%02x:%02x:%02x:%02x",
                     new_addr[0], new_addr[1], new_addr[2], new_addr[3], new_addr[4], new_addr[5]);
        }
    }
    return ESP_OK;
}

// keep_bonds: the camera bond fields (camera, cams, cam_poll_ds) belong to the camera task and its
// helpers below. A candidate built from an older snapshot (the portal's POST /api/config) must not
// roll them back, so the public commit takes them from the live config.
static esp_err_t commit_locked(const cfg_t *cand, bool keep_bonds)
{
    cfg_t tmp = *cand;
    if (keep_bonds) {
        tmp.camera = s_live.camera;
        memcpy(tmp.cams, s_live.cams, sizeof(tmp.cams));
        memcpy(tmp.cam_poll_ds, s_live.cam_poll_ds, sizeof(tmp.cam_poll_ds));
    }
    char msg[64];
    esp_err_t err = cfg_validate(&tmp, msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit rejected: %s", msg);
        return err;
    }
    tmp.magic   = CFG_MAGIC;
    tmp.version = CFG_VERSION;
    tmp.size    = sizeof(cfg_t);
    tmp.crc32   = blob_crc(&tmp);
    cfg_rid_seal(&tmp);               // the v7 tail's own CRC (spec 8.8.9)

    err = write_blob(&tmp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s -- keeping last-good in RAM", esp_err_to_name(err));
        return err;                 // live config untouched (spec 11.1)
    }
    publish_locked(&tmp);
    ESP_LOGI(TAG, "cfg committed (%u bytes)", (unsigned)sizeof(cfg_t));
    return ESP_OK;
}

esp_err_t cfg_commit(const cfg_t *cand)
{
    wlock();
    esp_err_t e = commit_locked(cand, true);
    wunlock();
    return e;
}

static int bond_index_of(const cfg_t *c, const uint8_t addr[6])
{
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        if (c->cams[i].brand[0] && memcmp(c->cams[i].addr, addr, 6) == 0) { return i; }
    }
    return -1;
}

// Copy a bond entry into camera.* (the active slot).
static void set_active_from(cfg_t *c, const cam_bond_t *b)
{
    strlcpy(c->camera.brand, b->brand, sizeof(c->camera.brand));
    memcpy(c->camera.addr, b->addr, 6);
    strlcpy(c->camera.name, b->name, sizeof(c->camera.name));
    c->camera.dji_device_id = b->dji_device_id;
    c->camera.bonded = b->brand[0] ? 1 : 0;
}

static esp_err_t set_camera_bond_impl(const char *brand, const uint8_t addr[6],
                                    const char *name, uint32_t dji_device_id)
{
    cfg_t c = s_live;
    int i = bond_index_of(&c, addr);
    const bool fresh = i < 0;                       // a brand-new bond inherits the live settings
    if (i < 0) {
        for (i = 0; i < CAM_MAX_BONDS && c.cams[i].brand[0]; i++) { }
        if (i >= CAM_MAX_BONDS) { return ESP_ERR_NO_MEM; }
        memset(&c.cams[i], 0, sizeof(c.cams[i]));
        c.cam_poll_ds[i] = CAM_POLL_DS_DEFAULT;   // new slot: default poll rate
    }
    cam_bond_t *b = &c.cams[i];
    strlcpy(b->brand, brand, sizeof(b->brand));
    memcpy(b->addr, addr, 6);
    strlcpy(b->name, name ? name : "", sizeof(b->name));
    if (dji_device_id)   { b->dji_device_id = dji_device_id; }
    else if (!b->dji_device_id && s_live.camera.dji_device_id &&
             memcmp(s_live.camera.addr, addr, 6) == 0) {
        b->dji_device_id = s_live.camera.dji_device_id;
    }
    const bool had_old = s_live.camera.bonded;
    uint8_t old_addr[6]; memcpy(old_addr, s_live.camera.addr, 6);
    const bool changed = !had_old || memcmp(old_addr, addr, 6) != 0;
    set_active_from(&c, b);            // newly paired camera becomes active
    if (changed) {
        esp_err_t e = switch_settings_locked(&c, had_old, old_addr, addr, fresh);
        if (e != ESP_OK) { return e; }
    }
    return commit_locked(&c, false);
}

esp_err_t cfg_set_camera_bond(const char *brand, const uint8_t addr[6],
                              const char *name, uint32_t dji_device_id)
{
    wlock();
    esp_err_t e = set_camera_bond_impl(brand, addr, name, dji_device_id);
    wunlock();
    return e;
}

static esp_err_t set_active_camera_impl(const uint8_t addr[6])
{
    cfg_t c = s_live;
    int i = bond_index_of(&c, addr);
    if (i < 0) { return ESP_ERR_NOT_FOUND; }
    if (memcmp(c.camera.addr, addr, 6) == 0 && c.camera.bonded) { return ESP_OK; }
    const bool had_old = c.camera.bonded;
    uint8_t old_addr[6]; memcpy(old_addr, c.camera.addr, 6);
    set_active_from(&c, &c.cams[i]);
    esp_err_t e = switch_settings_locked(&c, had_old, old_addr, addr, false);
    if (e != ESP_OK) { return e; }
    return commit_locked(&c, false);
}

esp_err_t cfg_set_active_camera(const uint8_t addr[6])
{
    wlock();
    esp_err_t e = set_active_camera_impl(addr);
    wunlock();
    return e;
}

static esp_err_t forget_camera_impl(const uint8_t addr[6])
{
    cfg_t c = s_live;
    int i = bond_index_of(&c, addr);
    if (i >= 0) { prof_erase(addr); nick_clear_locked(addr); nick_store(); }   // its settings and name go with it
    if (i >= 0) {
        for (int k = i; k + 1 < CAM_MAX_BONDS; k++) {
            c.cams[k] = c.cams[k + 1];
            c.cam_poll_ds[k] = c.cam_poll_ds[k + 1];
        }
        memset(&c.cams[CAM_MAX_BONDS - 1], 0, sizeof(c.cams[CAM_MAX_BONDS - 1]));
        c.cam_poll_ds[CAM_MAX_BONDS - 1] = 0;
    }
    if (memcmp(c.camera.addr, addr, 6) == 0) {
        if (c.cams[0].brand[0]) {
            set_active_from(&c, &c.cams[0]);
            switch_settings_locked(&c, false, NULL, c.cams[0].addr, false);   // the new active camera's own settings
        } else {
            memset(&c.camera, 0, sizeof(c.camera));
        }
    }
    return commit_locked(&c, false);
}

esp_err_t cfg_forget_camera(const uint8_t addr[6])
{
    wlock();
    esp_err_t e = forget_camera_impl(addr);
    wunlock();
    return e;
}

static esp_err_t clear_camera_impl(void)
{
    cfg_t c = s_live;
    for (int i = 0; i < CAM_MAX_BONDS; i++) { if (c.cams[i].brand[0]) { prof_erase(c.cams[i].addr); } }
    memset(s_nick, 0, sizeof(s_nick)); nick_store();
    memset(&c.camera, 0, sizeof(c.camera));
    memset(c.cams, 0, sizeof(c.cams));
    memset(c.cam_poll_ds, 0, sizeof(c.cam_poll_ds));
    return commit_locked(&c, false);
}

esp_err_t cfg_clear_camera(void)
{
    wlock();
    esp_err_t e = clear_camera_impl();
    wunlock();
    return e;
}

static esp_err_t cam_set_poll_impl(const uint8_t addr[6], uint8_t poll_ds)
{
    cfg_t c = s_live;
    int i = bond_index_of(&c, addr);
    if (i < 0) { return ESP_ERR_NOT_FOUND; }
    CLAMP(poll_ds, CAM_POLL_DS_MIN, CAM_POLL_DS_MAX);
    c.cam_poll_ds[i] = (uint8_t)(poll_ds | (c.cam_poll_ds[i] & CAM_POLL_WAKE));
    return commit_locked(&c, false);
}

static esp_err_t cam_set_wake_impl(const uint8_t addr[6], bool on)
{
    cfg_t c = s_live;
    int i = bond_index_of(&c, addr);
    if (i < 0) { return ESP_ERR_NOT_FOUND; }
    for (int k = 0; k < CAM_MAX_BONDS; k++) { c.cam_poll_ds[k] &= (uint8_t)~CAM_POLL_WAKE; }
    if (on) { c.cam_poll_ds[i] |= CAM_POLL_WAKE; }
    return commit_locked(&c, false);
}

esp_err_t cfg_cam_set_wake(const uint8_t addr[6], bool on)
{
    wlock();
    esp_err_t e = cam_set_wake_impl(addr, on);
    wunlock();
    return e;
}

esp_err_t cfg_cam_set_poll(const uint8_t addr[6], uint8_t poll_ds)
{
    wlock();
    esp_err_t e = cam_set_poll_impl(addr, poll_ds);
    wunlock();
    return e;
}

uint16_t cfg_active_poll_ms(void)
{
    uint8_t p = CAM_POLL_DS_DEFAULT;
    if (s_live.camera.bonded) {
        int i = bond_index_of(&s_live, s_live.camera.addr);
        if (i >= 0) { p = s_live.cam_poll_ds[i] & (uint8_t)~CAM_POLL_WAKE; }
    }
    if (p < CAM_POLL_DS_MIN || p > CAM_POLL_DS_MAX) { p = CAM_POLL_DS_DEFAULT; }
    return (uint16_t)p * 100u;   // deciseconds -> ms
}

int cfg_cam_count(void)
{
    int n = 0;
    for (int i = 0; i < CAM_MAX_BONDS; i++) { if (s_live.cams[i].brand[0]) { n++; } }
    return n;
}

bool cfg_cam_at(int i, cam_bond_t *out, uint8_t *poll_ds)
{
    if (i < 0 || i >= CAM_MAX_BONDS || !s_live.cams[i].brand[0]) { return false; }
    if (out) { *out = s_live.cams[i]; }
    if (poll_ds) {
        uint8_t p = s_live.cam_poll_ds[i] & (uint8_t)~CAM_POLL_WAKE;
        *poll_ds = (p >= CAM_POLL_DS_MIN && p <= CAM_POLL_DS_MAX) ? p : CAM_POLL_DS_DEFAULT;
    }
    return true;
}

esp_err_t cfg_cam_settings_get(const uint8_t addr[6], cfg_t *out, bool *stored)
{
    wlock();
    esp_err_t e = ESP_OK;
    if (bond_index_of(&s_live, addr) < 0) {
        e = ESP_ERR_NOT_FOUND;
    } else {
        *out = s_live;
        bool st = true;
        if (!(s_live.camera.bonded && memcmp(s_live.camera.addr, addr, 6) == 0)) {
            cfg_profile_t p;
            st = prof_load(addr, &p) && cfg_profile_apply(out, &p);   // else: inherits the live settings
        }
        if (stored) { *stored = st; }
    }
    wunlock();
    return e;
}

static esp_err_t cam_settings_commit_impl(const uint8_t addr[6], const cfg_t *cand)
{
    if (bond_index_of(&s_live, addr) < 0) { return ESP_ERR_NOT_FOUND; }
    if (s_live.camera.bonded && memcmp(s_live.camera.addr, addr, 6) == 0) {
        return commit_locked(cand, true);                  // the active camera: the ordinary commit
    }
    // Another camera: its sections go to its own key; the rest of cand (Wi-Fi, MSP, LED, ...) is
    // global and is committed to the live config, which keeps the ACTIVE camera's own sections.
    cfg_t tmp = *cand;
    tmp.camera = s_live.camera;
    memcpy(tmp.cams, s_live.cams, sizeof(tmp.cams));
    memcpy(tmp.cam_poll_ds, s_live.cam_poll_ds, sizeof(tmp.cam_poll_ds));
    char msg[64];
    esp_err_t e = cfg_validate(&tmp, msg, sizeof(msg));
    if (e != ESP_OK) { ESP_LOGE(TAG, "commit rejected: %s", msg); return e; }
    cfg_rid_seal(&tmp);
    cfg_profile_t p; cfg_profile_extract(&tmp, &p);
    e = prof_store(addr, &p);
    if (e != ESP_OK) { return e; }
    cfg_profile_t live_p; cfg_profile_extract(&s_live, &live_p);
    cfg_profile_apply(&tmp, &live_p);                      // back to the active camera's own sections
    cfg_t norm = tmp;                                      // skip the NVS write when no global field changed
    norm.magic = CFG_MAGIC; norm.version = CFG_VERSION; norm.size = sizeof(cfg_t);
    norm.crc32 = blob_crc(&norm); cfg_rid_seal(&norm);
    if (memcmp(&norm, &s_live, sizeof(norm)) == 0) { return ESP_OK; }
    return commit_locked(&tmp, true);
}

esp_err_t cfg_cam_settings_commit(const uint8_t addr[6], const cfg_t *cand)
{
    wlock();
    esp_err_t e = cam_settings_commit_impl(addr, cand);
    wunlock();
    return e;
}

bool cfg_rid_any_enabled(void)
{
    if (s_live.rid.enabled) { return true; }
    cfg_t t = s_live;
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        if (!s_live.cams[i].brand[0]) { continue; }
        cfg_profile_t p;
        if (prof_load(s_live.cams[i].addr, &p) && cfg_profile_apply(&t, &p) && t.rid.enabled) { return true; }
    }
    return false;
}

// "Wake at power-up" for bonded camera addr: its CAM_POLL_WAKE bit in the global per-bond table
// (set on the Binding tab, at most one camera). false if addr is not bonded.
bool cfg_cam_wake_on_boot(const uint8_t addr[6])
{
    int i = bond_index_of(&s_live, addr);
    return i >= 0 && (s_live.cam_poll_ds[i] & CAM_POLL_WAKE) != 0;
}

esp_err_t cfg_factory_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

// --------------------------------------------------------------------------
esp_err_t cfg_init(bool *out_cfg_reset, bool *out_erase_orphan_bond)
{
    if (!s_wlock) { s_wlock = xSemaphoreCreateMutexStatic(&s_wlock_buf); }
    if (out_cfg_reset)          { *out_cfg_reset = false; }
    if (out_erase_orphan_bond)  { *out_erase_orphan_bond = false; }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s -> erase", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) { return err; }

    cfg_t disk;
    size_t sz = sizeof(disk);
    nvs_handle_t h;
    bool good = false, migrated = false, rid_reset = false;

    if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
        // The blob may be smaller than sizeof(cfg_t) if written by older firmware.
        memset(&disk, 0, sizeof(disk));
        if (nvs_get_blob(h, KEY_BLOB, &disk, &sz) == ESP_OK &&
            sz >= offsetof(cfg_t, crc32) + sizeof(uint32_t) &&
            disk.magic == CFG_MAGIC) {

            if (disk.version == CFG_VERSION && sz == sizeof(cfg_t)) {
                good = (disk.crc32 == blob_crc(&disk));
                if (!good) { ESP_LOGE(TAG, "cfg CRC mismatch"); }
                // The Remote ID tail has its own CRC: a mismatch loads its DISABLED defaults
                // (a bit-flip must never switch on in-flight Wi-Fi reception) and re-commits.
                if (good && !cfg_rid_check(&disk)) {
                    ESP_LOGE(TAG, "cfg rid tail CRC mismatch -- Remote ID defaults (disabled) loaded");
                    rid_reset = true;
                }
            } else if (disk.version < CFG_VERSION) {
                // Older schema. Every field up to crc32 is unchanged across
                // versions, so the stored CRC still validates the head; the
                // v2+ tail (cams[]) was zeroed above and is filled by migration.
                if (disk.crc32 != blob_crc(&disk)) {
                    ESP_LOGE(TAG, "cfg v%u CRC mismatch -- not migrating", disk.version);
                } else if (cfg_migrate_chain(&disk)) {
                    good = migrated = true;
                    ESP_LOGW(TAG, "cfg migrated to v%u", CFG_VERSION);
                }
            } else {
                ESP_LOGE(TAG, "cfg v%u from newer firmware -- ignoring", disk.version);
            }
        }
        nvs_close(h);
    }

    if (good) {
        s_live = disk;
        s_ready = true;
        // Re-commit if we migrated (tail now present but flash still has old size).
        if (migrated || rid_reset || s_live.crc32 != blob_crc(&s_live) || s_live.version != CFG_VERSION) {
            s_live.version = CFG_VERSION;
            s_live.size = sizeof(cfg_t);
            s_live.crc32 = blob_crc(&s_live);
            cfg_rid_seal(&s_live);
            write_blob(&s_live);
        }
    } else {
        ESP_LOGW(TAG, "loading factory defaults, FLAG_CFG_RESET");
        cfg_load_defaults(&s_live);
        s_live.crc32 = blob_crc(&s_live);
        cfg_rid_seal(&s_live);
        s_ready = true;
        write_blob(&s_live);
        if (out_cfg_reset)         { *out_cfg_reset = true; }
        if (out_erase_orphan_bond) { *out_erase_orphan_bond = true; }  // spec 11.0
    }

    nick_load();
    ESP_LOGI(TAG, "cfg ready: brand=%s bonded=%u mode=%u baud=%lu",
             s_live.camera.brand[0] ? s_live.camera.brand : "-",
             s_live.camera.bonded, s_live.record.mode,
             (unsigned long)s_live.msp.baud);
    return ESP_OK;
}
