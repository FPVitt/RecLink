#include "cam_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static cam_state_t s_cs;
static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t  s_mux;

void cam_state_init(void)
{
    s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
    memset(&s_cs, 0, sizeof(s_cs));
    s_cs.link = CAM_LINK_IDLE;
    s_cs.recording = TRIBOOL_UNKNOWN;
    s_cs.busy = TRIBOOL_UNKNOWN;
}

void cam_state_get(cam_state_t *dst)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *dst = s_cs;
    xSemaphoreGive(s_mux);
}

void cam_state_publish(const cam_state_t *scratch)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cs = *scratch;
    xSemaphoreGive(s_mux);
}
