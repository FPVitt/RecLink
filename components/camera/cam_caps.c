#include "cam_caps.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static cam_caps_t s_caps;
static cam_preset_t s_presets[CAM_PRESETS_MAX];   // kept apart: the driver's working copy of cam_caps_t stays small
static uint8_t s_n_presets;
static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t s_mux;

void cam_caps_init(void)
{
    s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
}

void cam_caps_get(cam_caps_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_caps;
    xSemaphoreGive(s_mux);
}

uint16_t cam_caps_gen(void) { return *(volatile uint16_t *)&s_caps.gen; }

void cam_caps_publish(const cam_caps_t *c)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    // Bump the generation only on a real change, so the portal doesn't refetch
    // an identical list every time the driver re-asks the camera.
    if (memcmp(s_caps.have, c->have, sizeof(c->have)) != 0 ||
        memcmp(s_caps.bits, c->bits, sizeof(c->bits)) != 0) {
        memcpy(s_caps.have, c->have, sizeof(c->have));
        memcpy(s_caps.bits, c->bits, sizeof(c->bits));
        s_caps.gen++;
    }
    xSemaphoreGive(s_mux);
}

void cam_caps_publish_presets(const cam_preset_t *p, int n)
{
    if (n < 0) { n = 0; }
    if (n > CAM_PRESETS_MAX) { n = CAM_PRESETS_MAX; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_n_presets != n || (n && memcmp(s_presets, p, (size_t)n * sizeof(*p)) != 0)) {
        if (n) { memcpy(s_presets, p, (size_t)n * sizeof(*p)); }
        s_n_presets = (uint8_t)n;
        s_caps.gen++;
    }
    xSemaphoreGive(s_mux);
}

int cam_caps_presets(cam_preset_t *out, int max)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_n_presets < max ? s_n_presets : max;
    if (n > 0) { memcpy(out, s_presets, (size_t)n * sizeof(*out)); }
    xSemaphoreGive(s_mux);
    return n;
}
