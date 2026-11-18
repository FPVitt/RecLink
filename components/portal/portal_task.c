// portal_task.c -- SoftAP + HTTP + captive DNS lifecycle (spec 10.1), and the single owner of
// every esp_wifi_* call (spec 8.8.2.1 #4).
//
// The Wi-Fi / esp_netif / httpd init chain needs ~6 KB of stack, so it MUST run
// on portal_task's own stack -- never on the caller's (housekeeping's 2.5 KB
// stack overflowed doing this). portal_start()/portal_stop() are now cheap:
// they only create the task / signal it to tear down.
//
// Mode state machine (spec 8.8.2): OFF | AP | RID_RX | TEST. Other tasks only REQUEST a mode
// (portal_start / portal_stop for the AP, portal_rid_rx_request for Remote ID Wi-Fi reception,
// the reception test through rid_test_*) plus a task notification; this task alone drives the
// driver. The one call outside it stays portal_stop()'s esp_wifi_stop() on the arm edge.
// rid_task never calls the Wi-Fi driver.
#include "portal.h"
#include "cfg.h"
#include "platform.h"
#include "camera.h"
#include "applog.h"
#include "monoclock.h"
#include "rid.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal";

static TaskHandle_t   s_task;            // created once on the first portal_start(), then parked between sessions
static volatile bool  s_busy;            // a portal session is starting, up or tearing down
static volatile bool  s_want_stop;
static volatile bool  s_running;         // AP + HTTP actually up
static httpd_handle_t s_httpd;
static esp_netif_t   *s_ap_netif;
static uint32_t       s_opened_ms;
static volatile uint32_t s_last_client_ms;
// Remote ID Wi-Fi (spec 8.8.2): requested by housekeeping, run by this task only.
static volatile bool  s_rid_rx_req;      // housekeeping wants RID_RX
static volatile bool  s_rid_busy;        // a RID_RX / TEST session is starting, up or stopping
static bool           s_wifi_inited;     // driver initialised (this task only)

static StaticTask_t s_tcb;
static StackType_t  s_stack[6144 / sizeof(StackType_t)];

void http_api_register(httpd_handle_t server);   // http_api.c
void captive_dns_start(void);
void captive_dns_stop(void);
void portal_note_client_activity(void) { s_last_client_ms = now_ms(); }
static bool wifi_resident(void);

// ---- heavy bring-up, portal_task context only --------------------------
static void wifi_ap_up(void)
{
    const cfg_t *c = cfg_get();
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t wc = { 0 };
    if (c->wifi.ssid[0]) {
        strlcpy((char *)wc.ap.ssid, c->wifi.ssid, sizeof(wc.ap.ssid));
    } else {
        snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "RECLINK-%02X%02X", mac[4], mac[5]);
    }
    wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
    strlcpy((char *)wc.ap.password, c->wifi.password, sizeof(wc.ap.password));
    wc.ap.channel = c->wifi.channel ? c->wifi.channel : 1;
    wc.ap.max_connection = 3;                          // spec 10.2 (macOS may hold a stale assoc)
    wc.ap.authmode = strlen(c->wifi.password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.capable = true;                      // WPA2 + PMF-capable: newer macOS is picky
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // HT20 only -- clients complete the association / EAPOL handshake more
    // reliably than on HT40, and the portal needs no bandwidth.
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    // RF_PWR_* preset -> quarter-dBm units for esp_wifi_set_max_tx_power().
    // Re-applied every portal session since Wi-Fi is fully deinit/reinit each
    // time (spec 12.3).
    {
        static const int8_t k_wifi_pwr_q_dbm[] = { 8, 40, 80 };   // 2 / 10 / 20 dBm
        uint8_t p = c->wifi_tx_power > RF_PWR_MAX ? RF_PWR_MAX : c->wifi_tx_power;
        esp_err_t pe = esp_wifi_set_max_tx_power(k_wifi_pwr_q_dbm[p]);
        if (pe != ESP_OK) { ESP_LOGW(TAG, "wifi tx power: %s", esp_err_to_name(pe)); }
    }
    // Portal is bench-only and short-lived: keep the radio fully awake and
    // bias BT/Wi-Fi coexistence toward Wi-Fi so association isn't stalled by
    // the camera BLE link.
    esp_wifi_set_ps(WIFI_PS_NONE);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
    applog_note("portal AP '%s' up", wc.ap.ssid);
}

static void portal_bringup(void)
{
    if (!s_ap_netif) { s_ap_netif = esp_netif_create_default_wifi_ap(); }   // kept if resident
    // Portal address: 10.0.0.1 (gateway + DHCP server). Must be set before the
    // DHCP server hands out leases.
    {
        esp_netif_ip_info_t ip = {0};
        esp_netif_str_to_ip4("10.0.0.1",   &ip.ip);
        esp_netif_str_to_ip4("10.0.0.1",   &ip.gw);
        esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
        esp_netif_dhcps_stop(s_ap_netif);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));
        ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    }
    if (!s_wifi_inited) {
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&ic);
        if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) { ESP_LOGE(TAG, "wifi_init %s", esp_err_to_name(e)); }
        else {
            s_wifi_inited = true;
            // A resident driver cycles AP <-> NULL mode at every flight: keep its config in RAM so
            // that never becomes an NVS write per arming (the AP config is set every session anyway).
            if (wifi_resident()) { esp_wifi_set_storage(WIFI_STORAGE_RAM); }
        }
    }

    if (s_want_stop) { return; }     // armed while we were setting up: never bring the radio up
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

// Driver residency (spec 8.8.2.1 #5): with Remote ID Wi-Fi reception configured, the driver is
// initialised once and KEPT -- no 25-40 KB alloc/free churn and no init/deinit race at every
// takeoff. Without it, the portal's original full teardown applies.
static bool wifi_resident(void)
{
    const cfg_t *c = cfg_get();
    return c && c->rid.enabled && rid_available() && rid_mode_has_wifi(c->rid.mode) &&
           !bridge_flag_get(FLAG_RID_FAULT);
}

static void portal_teardown(void)
{
    captive_dns_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);   // give BLE its slots back
#endif
    // spec 12.3: the AP is off. The radio is then off, or -- only with Remote ID Wi-Fi reception
    // enabled and armed -- later in passive receive-only RID_RX (spec 8.8.2).
    esp_wifi_stop();
    if (wifi_resident()) {
        applog_note("portal down (Wi-Fi driver kept for Remote ID)");
        return;
    }
    esp_wifi_deinit();
    s_wifi_inited = false;
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; }
    applog_note("portal down");
}

static void portal_session(void)
{
    ESP_LOGI(TAG, "portal task: bringing up AP + HTTP");
    cam_ble_pause_scan(true);        // stop the camera hunt: a continuous BLE scan
    cam_throttle(true);              // starves Wi-Fi association on the single C3 radio
    portal_bringup();

    s_opened_ms = now_ms();
    s_last_client_ms = s_opened_ms;
    s_running = true;
    rid_set_wifi_state(RID_WIFI_AP);

    while (!s_want_stop) {
        // Woken immediately by portal_stop(); otherwise poll for the timeout.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (s_want_stop) { break; }
        // Remote ID reception test with Wi-Fi (spec 8.8.10): the AP is dropped (the phone
        // disconnects) once the POST has had time to answer. It never comes back from here --
        // only through portal_supervise(), and only while disarmed.
        if (rid_test_wifi() && now_ms() - rid_test_start_ms() >= 300) {
            applog_note("Remote ID reception test: AP down for Wi-Fi");
            break;
        }

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
    cam_throttle(false);
    cam_ble_pause_scan(false);       // resume the camera hunt
    s_running = false;
    rid_set_wifi_state(RID_WIFI_OFF);
}

// ---- Remote ID passive Wi-Fi reception (spec 8.8.2.2) ----------------------------------------
// Promiscuous callback, in the Wi-Fi task: bounded by the frame length, allocation-free,
// log-free. Management frames only; rx_ctrl.sig_len includes the 4-byte FCS.
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !buf) { return; }
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    unsigned sig = pk->rx_ctrl.sig_len;
    if (sig <= 4) { return; }
    rid_wifi_ingest(pk->payload, sig - 4u, (int8_t)pk->rx_ctrl.rssi);
}

static bool wifi_driver_up(void)
{
    if (s_wifi_inited) { return true; }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&ic);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "wifi_init (RID) %s", esp_err_to_name(e));
        return false;
    }
    s_wifi_inited = true;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // no NVS write per arming
    return true;
}

static void wifi_driver_down_if_not_resident(void)
{
    if (wifi_resident() || !s_wifi_inited) { return; }
    esp_wifi_deinit();
    s_wifi_inited = false;
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = NULL; }
}

// RID_RX / TEST: WIFI_MODE_NULL + start, promiscuous on management frames, channel 6 set AFTER
// start (it is not retained across a stop/start). Transmit-free: no AP, no STA connect, no scan,
// no NAN publish/subscribe, no 80211_tx. NO coexistence-preference call: on the C3 it is a no-op
// (VERIFY.md RID-ESP-COEX) -- the controller's automatic arbitration is all there is.
static bool passive_rx_start(void)
{
    if (!wifi_driver_up()) { return false; }
    if (esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK) { return false; }
    if (esp_wifi_start() != ESP_OK) { return false; }
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_err_t e = esp_wifi_set_promiscuous_filter(&f);
    if (e == ESP_OK) { e = esp_wifi_set_promiscuous_rx_cb(promisc_cb); }
    if (e == ESP_OK) { e = esp_wifi_set_promiscuous(true); }
    // The two reference sniffers enable promiscuous first, then set the channel (VERIFY.md
    // RID-ESP-SETCHAN); both satisfy "after esp_wifi_start()".
    if (e == ESP_OK) { e = esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE); }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "RID RX start: %s", esp_err_to_name(e));
        esp_wifi_set_promiscuous(false);
        esp_wifi_stop();
        return false;
    }
    return true;
}

static void passive_rx_stop(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
}

static void rid_rx_session(void)
{
    s_rid_busy = true;
    if (!passive_rx_start()) {
        // RID RX FAIL appears after its grace (rid_fault_mask). Retry slowly while still wanted;
        // never in a tight loop.
        applog_note("Remote ID Wi-Fi RX start failed");
        for (int i = 0; i < 25 && s_rid_rx_req; i++) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)); }
        s_rid_busy = false;
        return;
    }
    rid_set_wifi_state(RID_WIFI_RX);
    applog_note("Remote ID Wi-Fi RX up (ch 6, receive-only), heap free %lu",
                (unsigned long)esp_get_free_heap_size());
    while (s_rid_rx_req) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)); }
    passive_rx_stop();
    rid_set_wifi_state(RID_WIFI_OFF);
    wifi_driver_down_if_not_resident();
    applog_note("Remote ID Wi-Fi RX down");
    s_rid_busy = false;
}

// Reception test, Wi-Fi part (spec 8.8.2.1 #6 / 8.8.10): aborts at once on arming; never
// restarts the AP itself.
static void rid_test_session(void)
{
    s_rid_busy = true;
    rid_set_wifi_state(RID_WIFI_TEST);
    bool up = passive_rx_start();
    if (!up) { applog_note("Remote ID test: Wi-Fi RX start failed"); }
    while (rid_test_wifi() && !flight_may_be_airborne() && !s_rid_rx_req) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    if (flight_may_be_airborne()) { rid_test_abort(); }
    if (up) { passive_rx_stop(); }
    rid_set_wifi_state(RID_WIFI_OFF);
    wifi_driver_down_if_not_resident();
    s_rid_busy = false;
}

// One long-lived task. It used to vTaskDelete(NULL) itself after clearing s_task, which let a new
// portal_start() reuse the static stack/TCB while the old task was still exiting (a real hazard on
// the dual-core S3). Now it parks between sessions and the static memory is never reused.
// The mode state machine: AP sessions (s_busy), the Wi-Fi reception test, RID_RX, else park.
static void portal_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_busy) {
            portal_session();
            ulTaskNotifyTake(pdTRUE, 0);             // drop a stop wake-up that arrived after the loop ended
            s_busy = false;                          // only now may portal_start() begin the next session
            continue;
        }
        if (rid_test_wifi() && !flight_may_be_airborne()) { rid_test_session(); continue; }
        if (s_rid_rx_req) { rid_rx_session(); continue; }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // park until a request (stray wake-ups re-loop)
    }
}

static void ensure_task(void)
{
    if (!s_task) {
        s_task = xTaskCreateStatic(portal_task, "portal_task",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 5, s_stack, &s_tcb);
    } else {
        xTaskNotifyGive(s_task);     // wake the parked task
    }
}

// ---- cheap public API (any task) ------------------------------------
void portal_start(void)
{
    if (s_busy) { return; }          // already starting / up / tearing down
    s_want_stop = false;
    s_busy = true;
    ensure_task();
}

// housekeeping, every tick: RID_RX wanted? Only a change does anything (rid.enabled = 0 never
// creates the task or touches the radio).
void portal_rid_rx_request(bool want)
{
    if (want == s_rid_rx_req) { return; }
    s_rid_rx_req = want;
    if (want || s_task) { ensure_task(); }
}

void portal_wifi_kick(void) { if (s_task) { xTaskNotifyGive(s_task); } }
bool portal_ap_busy(void)   { return s_busy; }
bool portal_rid_busy(void)  { return s_rid_busy; }

void portal_stop(void)
{
    TaskHandle_t t = s_task;
    if (!t || !s_busy) { return; }
    s_want_stop = true;
    // Radio first, right here (esp_wifi_stop is thread-safe): the AP must be off within
    // ~100 ms of the armed edge (spec 12.4) and the session's own teardown can wait
    // behind a running HTTP handler (httpd_stop blocks until it returns).
    esp_wifi_stop();
    xTaskNotifyGive(t);              // wake it now -> finish the teardown
}

bool portal_active(void) { return s_running || s_busy; }

// ---- supervision, called from housekeeping at ~10 Hz ------------------
void portal_supervise(bool armed, bool trigger_open, bool button_long)
{
    static bool long_prev;

    // Hard rule (spec 13.3): never run the AP while armed.
    if (armed) {
        if (s_busy) { portal_stop(); }
        long_prev = button_long;
        return;
    }

    bool long_edge = button_long && !long_prev;
    long_prev = button_long;

    // Remote ID (spec 8.8.2.1 #4): the AP never starts while RID_RX is wanted, up or stopping, or
    // while a Wi-Fi reception test runs -- it returns only through here once those are done.
    bool rid_blocks = s_rid_rx_req || s_rid_busy || rid_test_wifi();

    if (!s_busy) {
        if ((trigger_open || long_edge) && !rid_blocks) { portal_start(); }
    } else if (long_edge) {
        portal_stop();               // long-press again = close early
    }
}
