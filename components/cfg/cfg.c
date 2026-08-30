#include "cfg.h"
#include "crc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define NS_CFG   "cfg"
#define KEY_BLOB "blob"

bool cfg_migrate_chain(cfg_t *c);   // cfg_migrate.c

static cfg_t s_live;                // last-good, always valid after cfg_init
static bool  s_ready;

// CRC covers everything from `magic` up to but not including `crc32`.
static uint32_t blob_crc(const cfg_t *c)
{
    return crc32_iso_hdlc((const uint8_t *)c, offsetof(cfg_t, crc32));
}

const cfg_t *cfg_get(void)
{
    return s_ready ? &s_live : NULL;
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

    CLAMP(c->record.mode, REC_OFF, REC_ARM_AND_SWITCH);
    CLAMP(c->record.arm_delay_ms, 0, 10000);
    CLAMP(c->record.stop_delay_ms, 0, 20000);
    CLAMP(c->record.verify_timeout_ms, 100, 5000);
    CLAMP(c->record.retry_count, 0, 10);
    CLAMP(c->record.sw.channel, 0, 14);
    CLAMP(c->record.sw.threshold_us, 900, 2100);

    CLAMP(c->preset.channel, 0, 14);
    CLAMP(c->preset.dwell_ms, 50, 5000);

    CLAMP(c->osd.refresh_hz, 1, 10);
    CLAMP(c->osd.corner, 0, 3);
    // New OSD tokens appended over time: if a blob has every classic token on
    // but none of the newer ones, turn the newer ones on too.
    if ((c->osd.fields_mask & 0xFFFu) == 0xFFFu) {
        c->osd.fields_mask |= ((1u << OSD_TOK_COUNT) - 1u);
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

    if (c->msp.baud != 115200 && c->msp.baud != 230400 &&
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

esp_err_t cfg_commit(const cfg_t *cand)
{
    cfg_t tmp = *cand;
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

    err = write_blob(&tmp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s -- keeping last-good in RAM", esp_err_to_name(err));
        return err;                 // s_live untouched (spec 11.1)
    }
    s_live = tmp;
    ESP_LOGI(TAG, "cfg committed (%u bytes)", (unsigned)sizeof(cfg_t));
    return ESP_OK;
}

esp_err_t cfg_set_camera_bond(const char *brand, const uint8_t addr[6],
                              const char *name, uint32_t dji_device_id)
{
    cfg_t c = s_live;
    strlcpy(c.camera.brand, brand, sizeof(c.camera.brand));
    memcpy(c.camera.addr, addr, 6);
    strlcpy(c.camera.name, name ? name : "", sizeof(c.camera.name));
    c.camera.bonded = 1;
    if (dji_device_id) { c.camera.dji_device_id = dji_device_id; }
    return cfg_commit(&c);
}

esp_err_t cfg_clear_camera(void)
{
    cfg_t c = s_live;
    memset(&c.camera, 0, sizeof(c.camera));
    // dji_device_id is intentionally kept? spec 11.0: forget zeroes cfg.camera.*
    // -> the id is regenerated on next first-ever pair. Zero it.
    return cfg_commit(&c);
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
    bool good = false;

    if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, KEY_BLOB, &disk, &sz) == ESP_OK &&
            sz >= offsetof(cfg_t, crc32) + sizeof(uint32_t) &&
            disk.magic == CFG_MAGIC) {

            if (disk.version == CFG_VERSION && sz == sizeof(cfg_t)) {
                good = (disk.crc32 == blob_crc(&disk));
                if (!good) { ESP_LOGE(TAG, "cfg CRC mismatch"); }
            } else if (disk.version < CFG_VERSION) {
                // Older schema: accept, migrate, re-commit below.
                if (cfg_migrate_chain(&disk)) {
                    good = true;
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
        // Re-commit if we migrated (version/size/crc now current but flash old).
        if (s_live.crc32 != blob_crc(&s_live) || s_live.version != CFG_VERSION) {
            s_live.version = CFG_VERSION;
            s_live.size = sizeof(cfg_t);
            s_live.crc32 = blob_crc(&s_live);
            write_blob(&s_live);
        }
    } else {
        ESP_LOGW(TAG, "loading factory defaults, FLAG_CFG_RESET");
        cfg_load_defaults(&s_live);
        s_live.crc32 = blob_crc(&s_live);
        s_ready = true;
        write_blob(&s_live);
        if (out_cfg_reset)         { *out_cfg_reset = true; }
        if (out_erase_orphan_bond) { *out_erase_orphan_bond = true; }  // spec 11.0
    }

    ESP_LOGI(TAG, "cfg ready: brand=%s bonded=%u mode=%u baud=%lu",
             s_live.camera.brand[0] ? s_live.camera.brand : "-",
             s_live.camera.bonded, s_live.record.mode,
             (unsigned long)s_live.msp.baud);
    return ESP_OK;
}
