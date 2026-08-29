#include "platform.h"

#include "esp_log.h"
#include "led_internal.h"

static const char *TAG = "platform";

void platform_init(void)
{
    rtcnv_boot_update();
    led_hw_init();
    button_hw_init();
    crash_stats_t s = rtcnv_get();
    ESP_LOGI(TAG, "boot #%lu reset=%lu wdt_recov=%lu brownout_recov=%lu",
             (unsigned long)s.boot_count, (unsigned long)s.last_reset_reason,
             (unsigned long)s.wdt_recoveries, (unsigned long)s.brownout_recoveries);
}
