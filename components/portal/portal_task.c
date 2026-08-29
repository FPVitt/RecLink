// portal_task.c -- SoftAP + HTTP + captive DNS lifecycle (spec 10.1).
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

static bool          s_active;
static httpd_handle_t s_httpd;
static esp_netif_t   *s_ap_netif;
static uint32_t       s_opened_ms;
static uint32_t       s_last_client_ms;
static bool           s_long_prev;

void http_api_register(httpd_handle_t server);   // http_api.c
void captive_dns_start(void);
void captive_dns_stop(void);
void portal_note_client_activity(void) { s_last_client_ms = now_ms(); }

static void wifi_ap_start(void)
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

void portal_start(void)
{
    if (s_active) { return; }
    ESP_LOGI(TAG, "opening portal");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&ic);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) { ESP_LOGE(TAG, "wifi_init %s", esp_err_to_name(e)); }

    wifi_ap_start();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 24;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        http_api_register(s_httpd);
    }
    captive_dns_start();

    s_opened_ms = now_ms();
    s_last_client_ms = s_opened_ms;
    s_active = true;
    led_request(LED_PORTAL);
}

void portal_stop(void)
{
    if (!s_active) { return; }
    ESP_LOGI(TAG, "closing portal");
    captive_dns_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();                 // spec 12.3: radio fully off
    esp_wifi_deinit();
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; }
    s_active = false;
    applog_note("portal down");
}

bool portal_active(void) { return s_active; }

void portal_supervise(bool armed, bool trigger_open, bool button_long)
{
    // Hard rule (spec 13.3): never run the AP while armed.
    if (armed) {
        if (s_active) { portal_stop(); }
        s_long_prev = button_long;
        return;
    }

    bool long_edge = button_long && !s_long_prev;
    s_long_prev = button_long;

    if (!s_active) {
        if (trigger_open || long_edge) { portal_start(); }
        return;
    }

    // long-press again = close early
    if (long_edge) { portal_stop(); return; }

    // auto-close after timeout unless a client was recently active (spec 10.1)
    const cfg_t *c = cfg_get();
    uint32_t timeout_ms = (uint32_t)c->wifi.portal_timeout_min * 60u * 1000u;
    uint32_t idle = now_ms() - s_last_client_ms;
    if ((now_ms() - s_opened_ms) > timeout_ms && idle > 60000 && !trigger_open) {
        portal_stop();
    }
}
