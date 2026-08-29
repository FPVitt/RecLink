// portal_task.c -- SoftAP + HTTP + captive DNS lifecycle (spec 10.1).
//
// The Wi-Fi / esp_netif / httpd init chain needs ~6 KB of stack, so it MUST run
// on portal_task's own stack -- never on the caller's (housekeeping's 2.5 KB
// stack overflowed doing this). portal_start()/portal_stop() are now cheap:
// they only create the task / signal it to tear down.
#include "portal.h"
#include "cfg.h"
#include "platform.h"
#include "applog.h"
#include "monoclock.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal";

static TaskHandle_t   s_task;
static volatile bool  s_want_stop;
static volatile bool  s_running;         // AP + HTTP actually up
static httpd_handle_t s_httpd;
static esp_netif_t   *s_ap_netif;
static uint32_t       s_opened_ms;
static volatile uint32_t s_last_client_ms;

static StaticTask_t s_tcb;
static StackType_t  s_stack[6144 / sizeof(StackType_t)];

void http_api_register(httpd_handle_t server);   // http_api.c
void captive_dns_start(void);
void captive_dns_stop(void);
void portal_note_client_activity(void) { s_last_client_ms = now_ms(); }

// ---- heavy bring-up, portal_task context only --------------------------
static void wifi_ap_up(void)
{
    const cfg_t *c = cfg_get();
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t wc = { 0 };
    if (c->wifi.ssid[0]) {
        strlcpy((char *)wc.ap.ssid, c->wifi.ssid, sizeof(wc.ap.ssid));
    } else {
        snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "CAMBRIDGE-%02X%02X", mac[4], mac[5]);
    }
    wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
    strlcpy((char *)wc.ap.password, c->wifi.password, sizeof(wc.ap.password));
    wc.ap.channel = c->wifi.channel ? c->wifi.channel : 1;
    wc.ap.max_connection = 2;                          // spec 10.2
    wc.ap.authmode = strlen(c->wifi.password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    applog_note("portal AP '%s' up", wc.ap.ssid);
}

static void portal_bringup(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&ic);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) { ESP_LOGE(TAG, "wifi_init %s", esp_err_to_name(e)); }

    wifi_ap_up();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 24;
    hc.lru_purge_enable = true;
    hc.stack_size = 8192;                 // OTA handler buffers + flash calls
    hc.recv_wait_timeout = 20;            // slow SoftAP + flash-stalled receiver
    hc.send_wait_timeout = 20;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        http_api_register(s_httpd);
    }
    captive_dns_start();
}

static void portal_teardown(void)
{
    captive_dns_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();                 // spec 12.3: radio fully off
    esp_wifi_deinit();
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; }
    applog_note("portal down");
}

static void portal_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "portal task: bringing up AP + HTTP");
    portal_bringup();

    s_opened_ms = now_ms();
    s_last_client_ms = s_opened_ms;
    s_running = true;
    led_request(LED_PORTAL);

    while (!s_want_stop) {
        // Woken immediately by portal_stop(); otherwise poll for the timeout.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (s_want_stop) { break; }

        const cfg_t *c = cfg_get();
        uint32_t timeout_ms = (uint32_t)c->wifi.portal_timeout_min * 60u * 1000u;
        uint32_t up   = now_ms() - s_opened_ms;
        uint32_t idle = now_ms() - s_last_client_ms;
        if (up > timeout_ms && idle > 60000) {
            applog_note("portal idle timeout");
            break;
        }
    }

    portal_teardown();
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---- cheap public API (any task) ------------------------------------
void portal_start(void)
{
    if (s_task) { return; }          // already running / starting
    s_want_stop = false;
    s_task = xTaskCreateStatic(portal_task, "portal_task",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 5, s_stack, &s_tcb);
}

void portal_stop(void)
{
    TaskHandle_t t = s_task;
    if (!t) { return; }
    s_want_stop = true;
    xTaskNotifyGive(t);              // wake it now -> fast teardown
}

bool portal_active(void) { return s_running || s_task != NULL; }

// ---- supervision, called from housekeeping at ~10 Hz ------------------
void portal_supervise(bool armed, bool trigger_open, bool button_long)
{
    static bool long_prev;

    // Hard rule (spec 13.3): never run the AP while armed.
    if (armed) {
        if (s_task) { portal_stop(); }
        long_prev = button_long;
        return;
    }

    bool long_edge = button_long && !long_prev;
    long_prev = button_long;

    if (!s_task) {
        if (trigger_open || long_edge) { portal_start(); }
    } else if (long_edge) {
        portal_stop();               // long-press again = close early
    }
}
