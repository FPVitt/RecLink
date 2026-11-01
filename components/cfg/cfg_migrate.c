// cfg_migrate.c -- explicit per-version migration chain (spec 11.0: NEVER wipe on
// version mismatch). Each step upgrades vN -> vN+1 in place on an
// already-magic-valid blob whose CRC has been checked against its own version.
#include "cfg.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cfg.mig";

// v1 -> v2: multi-camera bond list (cams[]) appended after crc32. The single v1
// `camera` becomes bond slot 0 and stays the active camera. cams[] was zeroed by
// the caller before migration runs.
static void migrate_1_to_2(cfg_t *c)
{
    if (c->camera.bonded && c->camera.brand[0]) {
        strlcpy(c->cams[0].brand, c->camera.brand, sizeof(c->cams[0].brand));
        memcpy(c->cams[0].addr, c->camera.addr, 6);
        strlcpy(c->cams[0].name, c->camera.name, sizeof(c->cams[0].name));
        c->cams[0].dji_device_id = c->camera.dji_device_id;
    }
    c->version = 2;
}

// v2 -> v3: per-camera BLE poll interval (cam_poll_ds[]) appended at the very end,
// after cams[]. cams[] layout is unchanged so nothing needs re-spacing; the
// caller zeroed the tail, so just fill in the default and bump the version.
static void migrate_2_to_3(cfg_t *c)
{
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        if (c->cam_poll_ds[i] == 0) { c->cam_poll_ds[i] = CAM_POLL_DS_DEFAULT; }
    }
    c->version = 3;
}

// v3 -> v4: RF transmit power presets (wifi_tx_power / ble_tx_power) appended at
// the very end. RF_PWR_LOW == 0, so the zeroed tail can't be told apart from an
// explicit "Low" choice -- set both to MAX explicitly to match the prior
// (implicit, unconfigurable) always-max behaviour.
static void migrate_3_to_4(cfg_t *c)
{
    c->wifi_tx_power = RF_PWR_MAX;
    c->ble_tx_power  = RF_PWR_MAX;
    c->version = 4;
}

// v4 -> v5: per-zone GoPro settings (gp_preset[]) appended at the very end. The
// caller zeroed the tail, i.e. every zone has n == 0 ("push nothing"), so there
// is nothing to fill in. A GoPro zone's old preset ID is no longer used.
static void migrate_4_to_5(cfg_t *c)
{
    c->version = 5;
}

// v5 -> v6: variable zone count (n_zones, zones 4.. in the tail). Existing blobs
// have exactly the 3 zones they always had.
static void migrate_5_to_6(cfg_t *c)
{
    c->n_zones = PRESET_ZONES_DEFAULT;
    c->version = 6;
}

typedef void (*migrate_fn)(cfg_t *);

// index 0 -> migrate v1 to v2, index 1 -> v2 to v3, etc.
#define N_MIGRATIONS 5
#if N_MIGRATIONS > 0
static const migrate_fn k_chain[N_MIGRATIONS] = { migrate_1_to_2, migrate_2_to_3, migrate_3_to_4, migrate_4_to_5, migrate_5_to_6 };
#endif

// Returns true if the blob is now at CFG_VERSION; false if no path exists.
bool cfg_migrate_chain(cfg_t *c)
{
    while (c->version < CFG_VERSION) {
#if N_MIGRATIONS > 0
        unsigned step = c->version - 1u;
        if (step >= N_MIGRATIONS || !k_chain[step]) {
            ESP_LOGE(TAG, "no migration from v%u", c->version);
            return false;
        }
        ESP_LOGW(TAG, "migrating cfg v%u -> v%u", c->version, c->version + 1);
        k_chain[step](c);
#else
        ESP_LOGE(TAG, "no migration path from cfg v%u", c->version);
        return false;
#endif
    }
    if (c->version > CFG_VERSION) {
        ESP_LOGE(TAG, "cfg v%u newer than firmware v%u -- refusing", c->version, CFG_VERSION);
        return false;
    }
    c->size = sizeof(cfg_t);
    return true;
}
