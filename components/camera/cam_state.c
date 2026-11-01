#include "cam_state.h"
#include "cam_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"

// Single-writer model (spec 3.4). `s_scratch` is mutated ONLY from cam_task
// context -- by cam_task itself and by the driver callbacks it invokes, which
// all run on cam_task. There is therefore no concurrency on the scratch. The
// mutex guards only the publish into `s_shared`, which external readers
// (osd_task, ctrl_task, portal, housekeeping) copy out under.
static cam_state_t s_shared;
static cam_state_t s_scratch;
static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t  s_mux;
static TaskHandle_t s_owner;      // set on first cam_scratch() call, for the assert

void cam_state_init(void)
{
    s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
    cam_caps_init();
    memset(&s_scratch, 0, sizeof(s_scratch));
    s_scratch.link = CAM_LINK_IDLE;
    s_scratch.recording = TRIBOOL_UNKNOWN;
    s_scratch.busy = TRIBOOL_UNKNOWN;
    s_shared = s_scratch;
}

void cam_state_get(cam_state_t *dst)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *dst = s_shared;
    xSemaphoreGive(s_mux);
}

void cam_state_get_link(cam_link_state_t *link, uint32_t *last_rx_ms)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *link = s_shared.link;
    *last_rx_ms = s_shared.last_rx_ms;
    xSemaphoreGive(s_mux);
}

void cam_state_get_brief(cam_link_state_t *link, tribool_t *recording)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *link = s_shared.link;
    *recording = s_shared.recording;
    xSemaphoreGive(s_mux);
}

cam_state_t *cam_scratch(void)
{
    if (s_owner == NULL) {
        s_owner = xTaskGetCurrentTaskHandle();
    } else if (s_owner != xTaskGetCurrentTaskHandle()) {
        // A second task touched the scratch -- the one-writer rule is broken.
        ESP_LOGE("cam_state", "cam_scratch() from wrong task!");
        assert(0);
    }
    return &s_scratch;
}

void cam_state_commit(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_shared = s_scratch;
    xSemaphoreGive(s_mux);
}
