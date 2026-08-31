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

typedef void (*migrate_fn)(cfg_t *);

// index 0 -> migrate v1 to v2, etc.
#define N_MIGRATIONS 1
#if N_MIGRATIONS > 0
static const migrate_fn k_chain[N_MIGRATIONS] = { migrate_1_to_2 };
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
