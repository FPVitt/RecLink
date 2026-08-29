// twdt.c -- thin wrapper over esp_task_wdt. Each of the four critical tasks
// (msp/cam/ctrl/housekeeping) registers itself and feeds once per loop. A stall
// -> panic -> reboot (spec 3.5); we do NOT attempt selective task restart.
#include "platform.h"

#include "esp_task_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "twdt";
static TaskHandle_t s_handles[WDT_TASK_COUNT];

void twdt_register(wdt_task_t which)
{
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    s_handles[which] = h;
    esp_err_t err = esp_task_wdt_add(h);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "wdt_add(%d) = %s", which, esp_err_to_name(err));
    }
}

void twdt_feed(wdt_task_t which)
{
    (void)which;
    esp_task_wdt_reset();
}
