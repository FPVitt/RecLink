#include "cam_caps.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static cam_caps_t s_caps;
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
