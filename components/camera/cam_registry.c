#include "camera.h"
#include <string.h>
#include "esp_log.h"

#define MAX_DRIVERS 4
static const camera_driver_t *s_drv[MAX_DRIVERS];
static int s_n;

// cbs are shared: every driver reports through the same sink owned by cam_task.
static camera_cbs_t s_cbs;
void cam_registry_set_cbs(const camera_cbs_t *c) { s_cbs = *c; }
const camera_cbs_t *camera_cbs(void) { return &s_cbs; }

void camera_register(const camera_driver_t *drv)
{
    if (s_n < MAX_DRIVERS) {
        s_drv[s_n++] = drv;
        ESP_LOGI("cam.reg", "registered driver '%s'", drv->name);
    }
}

const camera_driver_t *camera_by_name(const char *name)
{
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_drv[i]->name, name) == 0) { return s_drv[i]; }
    }
    return NULL;
}

const camera_driver_t *camera_match_adv(const ble_adv_t *adv)
{
    for (int i = 0; i < s_n; i++) {
        if (s_drv[i]->adv_match && s_drv[i]->adv_match(adv)) { return s_drv[i]; }
    }
    return NULL;
}
