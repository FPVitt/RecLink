// wifi_ap.c -- always-on SoftAP at 10.0.0.1 + captive DNS (lifted from the
// original portal: same addressing, so http://10.0.0.1 works with either image).
#include "app.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ap";
static char s_ssid[33];

const char *wifi_ap_ssid(void) { return s_ssid; }

int wifi_ap_clients(void)
{
    wifi_sta_list_t l;
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

esp_err_t wifi_ap_start(void)
{
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip = {0};
    esp_netif_str_to_ip4("10.0.0.1", &ip.ip);
    esp_netif_str_to_ip4("10.0.0.1", &ip.gw);
    esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
    esp_netif_dhcps_stop(ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "%s-%02X%02X", CONFIG_DBG_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, s_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)wc.ap.password, CONFIG_DBG_AP_PASSWORD, sizeof(wc.ap.password));
    wc.ap.channel = CONFIG_DBG_AP_CHANNEL;
    wc.ap.max_connection = 3;
    wc.ap.authmode = strlen(CONFIG_DBG_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.capable = true;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_ps(WIFI_PS_NONE);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);   // BLE link is the subject here
#endif
    ESP_LOGI(TAG, "AP '%s' up at 10.0.0.1", s_ssid);
    return ESP_OK;
}

// ---- captive DNS: answer every A query with 10.0.0.1 ----------------------
static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "dns socket/bind failed");
        if (s >= 0) { close(s); }
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) { continue; }
        buf[2] |= 0x80;                      // QR = response
        buf[3] = 0x00;
        buf[6] = 0; buf[7] = 1;              // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int q = 12;
        while (q < n && buf[q] != 0) { q += buf[q] + 1; }
        q += 1 + 4;
        if (q + 16 > (int)sizeof(buf) || q > n) { continue; }
        uint8_t *a = buf + q;
        const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 0, 0, 1 };
        memcpy(a, ans, sizeof(ans));
        sendto(s, buf, q + 16, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_dns_start(void) { xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL); }
