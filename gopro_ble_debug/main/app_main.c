// app_main.c -- GOPRO-BLE-DEBUG entry point.
//
// Coexistence contract with the original RecLink firmware (same flash):
//   * never erases NVS and never touches the original's "cfg" namespace;
//   * shares the NimBLE "nimble_bond" namespace (bonds work in both images);
//   * marks itself valid only after the Wi-Fi AP + web server are up -- if it
//     can't get that far, the bootloader rolls back to the original on reset.
#include "app.h"
#include "dbg_ble.h"
#include "evlog.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

// Crash-loop guard. Rollback only protects the first boot after an OTA; once
// this image is valid, a bug that panics on every boot would leave the board
// stuck in debug firmware. So: CRASH_LIMIT consecutive panic/watchdog resets
// (each within CRASH_WINDOW_S of boot) -> boot the other slot (the original).
#define CRASH_LIMIT     3
#define CRASH_WINDOW_S  60
static RTC_NOINIT_ATTR uint32_t s_crash_magic, s_crash_count;

static void crash_guard(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT || why == ESP_RST_WDT;
    if (s_crash_magic != 0xDB6C0DE5u) { s_crash_magic = 0xDB6C0DE5u; s_crash_count = 0; }
    s_crash_count = crash ? s_crash_count + 1 : 0;
    if (s_crash_count < CRASH_LIMIT) { return; }
    s_crash_count = 0;
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (other && esp_ota_get_partition_description(other, &d) == ESP_OK &&
        esp_ota_set_boot_partition(other) == ESP_OK) {
        ESP_LOGE(TAG, "%d crash resets in a row -> booting %s (%s %s)", CRASH_LIMIT, other->label,
                 d.project_name, d.version);
        esp_restart();
    }
}

static void crash_window_over(void *arg) { (void)arg; s_crash_count = 0; }

void app_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        evlog_text(EV_SYS, "firmware marked valid (%s)", run->label);
    }
}

// Status LED: slow blink = no camera link, fast blink = connecting, solid = connected.
static void led_task(void *arg)
{
    (void)arg;
    const int pin = CONFIG_DBG_LED_GPIO;
#ifdef CONFIG_DBG_LED_ACTIVE_LOW
    const int on = 0;
#else
    const int on = 1;
#endif
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    for (uint32_t t = 0;; t++) {
        dbg_status_t s;
        dbg_ble_status(&s);
        int lit = s.link == DBG_LINK_CONNECTED ? 1
                : s.link == DBG_LINK_CONNECTING ? (t & 1)
                : (t % 10) < 1;
        gpio_set_level(pin, lit ? on : !on);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    evlog_init();
    syslog_hook_init();
    ESP_LOGI(TAG, "GOPRO-BLE-DEBUG starting (reset reason %d)", esp_reset_reason());
    crash_guard();
    {
        static esp_timer_handle_t t;
        const esp_timer_create_args_t a = { .callback = crash_window_over, .name = "crashwin" };
        if (esp_timer_create(&a, &t) == ESP_OK) { esp_timer_start_once(t, CRASH_WINDOW_S * 1000000ULL); }
    }

    // NVS holds the ORIGINAL firmware's settings: never erase it. If it can't
    // be opened we run without persistent bonds rather than wipe anything.
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s -- continuing WITHOUT bond storage (NVS left untouched)",
                 esp_err_to_name(e));
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    dbg_ble_init();
    ESP_ERROR_CHECK(wifi_ap_start());
    captive_dns_start();
    ESP_ERROR_CHECK(web_start());
    app_mark_valid();

    // 3 KB: gpio_reset_pin()/gpio_set_direction() log through ESP_LOG.
    if (CONFIG_DBG_LED_GPIO >= 0) { xTaskCreate(led_task, "led", 3072, NULL, 1, NULL); }
    evlog_text(EV_SYS, "ready: join Wi-Fi '%s', open http://10.0.0.1", wifi_ap_ssid());
    ESP_LOGI(TAG, "boot headroom: main stack %u B unused, heap %u B free (min %u)",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}
