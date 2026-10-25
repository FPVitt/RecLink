// web.c -- HTTP API + embedded UI. Bench tool: no auth beyond the AP's WPA2.
//
// The browser does all Open GoPro message building and decoding; this side is
// a transport: write bytes to a handle (optionally GoPro-packetized), read a
// handle, toggle CCCDs, and stream every packet back through /api/events.
#include "app.h"
#include "dbg_ble.h"
#include "evlog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"

static const char *TAG = "web";

#define ASSET(sym) \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_gz_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_gz_end");
ASSET(index_html)
ASSET(app_js)
ASSET(ops_js)
ASSET(catalog_js)
ASSET(style_css)

// ---- helpers ---------------------------------------------------------------
static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t send_result(httpd_req_t *r, esp_err_t e)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    if (e != ESP_OK) {
        cJSON_AddStringToObject(j, "error", e == ESP_ERR_INVALID_STATE ? "not connected / wrong state"
                                            : esp_err_to_name(e));
        httpd_resp_set_status(r, "409 Conflict");
    }
    return send_json(r, j);
}

static cJSON *read_json(httpd_req_t *r)
{
    size_t len = r->content_len;
    if (len == 0) { return cJSON_CreateObject(); }
    if (len > 20000) { return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { return NULL; }
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) { continue; }
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static int jint(const cJSON *j, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    if (cJSON_IsNumber(v)) { return v->valueint; }
    if (cJSON_IsBool(v))   { return cJSON_IsTrue(v); }
    return def;
}

static const char *jstr(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// "AA:BB:CC:DD:EE:FF" (display order, MSB first) -> NimBLE little-endian.
static bool parse_addr(const char *s, uint8_t out[6])
{
    unsigned b[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) { return false; }
    for (int i = 0; i < 6; i++) { out[5 - i] = (uint8_t)b[i]; }
    return true;
}

static void fmt_addr(char *dst, const uint8_t a[6])
{
    sprintf(dst, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static int parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0, hi = -1;
    for (; s && *s; s++) {
        int c = *s, v;
        if (c >= '0' && c <= '9') { v = c - '0'; }
        else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
        else if (c == ' ' || c == ':' || c == ',' || c == '-' || c == '\n' || c == '\t') { continue; }
        else { return -1; }
        if (hi < 0) { hi = v; continue; }
        if (n >= cap) { return -1; }
        out[n++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    return hi < 0 ? n : -1;
}

static void uuid_str(const ble_uuid_any_t *u, char *dst, size_t cap)
{
    char tmp[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&u->u, tmp);
    strlcpy(dst, tmp, cap);
}

static void add_desc(cJSON *o, const esp_partition_t *p)
{
    esp_app_desc_t d;
    cJSON_AddStringToObject(o, "label", p->label);
    cJSON_AddNumberToObject(o, "addr", p->address);
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        cJSON_AddBoolToObject(o, "valid", true);
        cJSON_AddStringToObject(o, "project", d.project_name);
        cJSON_AddStringToObject(o, "version", d.version);
        cJSON_AddStringToObject(o, "idf", d.idf_ver);
        char dt[40];
        snprintf(dt, sizeof(dt), "%s %s", d.date, d.time);
        cJSON_AddStringToObject(o, "built", dt);
    } else {
        cJSON_AddBoolToObject(o, "valid", false);
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
        const char *n = st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending-verify"
                      : st == ESP_OTA_IMG_INVALID ? "invalid" : st == ESP_OTA_IMG_ABORTED ? "aborted"
                      : st == ESP_OTA_IMG_NEW ? "new" : "undefined";
        cJSON_AddStringToObject(o, "ota_state", n);
    }
}

// ---- GET /api/status ----------------------------------------------------
static esp_err_t h_status(httpd_req_t *r)
{
    dbg_status_t s;
    dbg_ble_status(&s);
    cJSON *j = cJSON_CreateObject();
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(j, "fw", ad->version);
    cJSON_AddStringToObject(j, "project", ad->project_name);
    cJSON_AddStringToObject(j, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(j, "ssid", wifi_ap_ssid());
    cJSON_AddNumberToObject(j, "clients", wifi_ap_clients());

    cJSON *p = cJSON_AddObjectToObject(j, "running");
    add_desc(p, esp_ota_get_running_partition());
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) { add_desc(cJSON_AddObjectToObject(j, "other"), other); }

    cJSON *b = cJSON_AddObjectToObject(j, "ble");
    cJSON_AddBoolToObject(b, "synced", s.synced);
    cJSON_AddBoolToObject(b, "scanning", s.scanning);
    cJSON_AddStringToObject(b, "link", s.link == DBG_LINK_CONNECTED ? "connected"
                                     : s.link == DBG_LINK_CONNECTING ? "connecting" : "idle");
    char a[18];
    fmt_addr(a, s.peer);
    cJSON_AddStringToObject(b, "peer", s.link == DBG_LINK_IDLE ? "" : a);
    cJSON_AddNumberToObject(b, "peer_type", s.peer_type);
    cJSON_AddNumberToObject(b, "mtu", s.mtu);
    cJSON_AddBoolToObject(b, "encrypted", s.encrypted);
    cJSON_AddBoolToObject(b, "bonded", s.bonded);
    cJSON_AddBoolToObject(b, "authenticated", s.authenticated);
    cJSON_AddNumberToObject(b, "key_size", s.key_size);
    cJSON_AddNumberToObject(b, "rssi", s.rssi);
    cJSON_AddBoolToObject(b, "keepalive", s.keepalive);
    cJSON_AddNumberToObject(b, "keepalive_ms", s.keepalive_ms);
    cJSON_AddNumberToObject(b, "tx", s.tx_pkts);
    cJSON_AddNumberToObject(b, "rx", s.rx_pkts);
    cJSON_AddNumberToObject(b, "tx_err", s.tx_errs);
    cJSON_AddNumberToObject(b, "last_disc_reason", s.last_disc_reason);
    cJSON_AddNumberToObject(b, "connected_s", s.link == DBG_LINK_CONNECTED
                            ? (double)((esp_timer_get_time() / 1000 - s.connected_ms) / 1000) : 0);
    cJSON *au = cJSON_AddObjectToObject(b, "auto");
    cJSON_AddBoolToObject(au, "secure", s.auto_secure);
    cJSON_AddBoolToObject(au, "discover", s.auto_discover);
    cJSON_AddBoolToObject(au, "subscribe", s.auto_subscribe);
    cJSON_AddNumberToObject(j, "ev_next", evlog_next_seq());
    return send_json(r, j);
}

// ---- scan / devices -------------------------------------------------
static esp_err_t h_scan(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) { return send_result(r, ESP_ERR_INVALID_ARG); }
    bool on = jint(j, "on", 1);
    if (jint(j, "clear", 0)) { dbg_ble_devices_clear(); }
    esp_err_t e = dbg_ble_scan(on, jint(j, "secs", 10), jint(j, "active", 1));
    cJSON_Delete(j);
    return send_result(r, e);
}

static void hex_into(char *dst, const uint8_t *p, int n)
{
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < n; i++) { dst[2 * i] = hx[p[i] >> 4]; dst[2 * i + 1] = hx[p[i] & 15]; }
    dst[2 * n] = '\0';
}

static esp_err_t h_devices(httpd_req_t *r)
{
    static dbg_dev_t d[20];
    int n = dbg_ble_devices(d, 20);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    cJSON *j = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(j, "devices");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char s[64];
        fmt_addr(s, d[i].addr);
        cJSON_AddStringToObject(o, "addr", s);
        cJSON_AddNumberToObject(o, "type", d[i].addr_type);
        cJSON_AddNumberToObject(o, "rssi", d[i].rssi);
        cJSON_AddStringToObject(o, "name", d[i].name);
        cJSON_AddBoolToObject(o, "gopro", d[i].gopro);
        cJSON_AddBoolToObject(o, "bonded", d[i].bonded);
        cJSON_AddNumberToObject(o, "age_ms", now - d[i].last_ms);
        hex_into(s, d[i].adv, d[i].adv_len); cJSON_AddStringToObject(o, "adv", s);
        hex_into(s, d[i].rsp, d[i].rsp_len); cJSON_AddStringToObject(o, "rsp", s);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(r, j);
}

// ---- link ------------------------------------------------------------
static esp_err_t h_connect(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    uint8_t a[6];
    if (!j || !parse_addr(jstr(j, "addr"), a)) { cJSON_Delete(j); return send_result(r, ESP_ERR_INVALID_ARG); }
    if (cJSON_GetObjectItem(j, "auto_secure")) {
        dbg_ble_set_auto(jint(j, "auto_secure", 1), jint(j, "auto_discover", 1), jint(j, "auto_subscribe", 1));
    }
    esp_err_t e = dbg_ble_connect(a, (uint8_t)jint(j, "type", 0));
    cJSON_Delete(j);
    return send_result(r, e);
}

static esp_err_t h_disconnect(httpd_req_t *r) { dbg_ble_disconnect(); return send_result(r, ESP_OK); }
static esp_err_t h_secure(httpd_req_t *r)     { return send_result(r, dbg_ble_secure()); }
static esp_err_t h_discover(httpd_req_t *r)   { return send_result(r, dbg_ble_discover()); }
static esp_err_t h_mtu(httpd_req_t *r)        { return send_result(r, dbg_ble_mtu()); }
static esp_err_t h_sub_all(httpd_req_t *r)    { return send_result(r, dbg_ble_subscribe_all()); }

static esp_err_t h_auto(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) { return send_result(r, ESP_ERR_INVALID_ARG); }
    dbg_ble_set_auto(jint(j, "secure", 1), jint(j, "discover", 1), jint(j, "subscribe", 1));
    cJSON_Delete(j);
    return send_result(r, ESP_OK);
}

static esp_err_t h_gatt(httpd_req_t *r)
{
    static dbg_gatt_t g;
    dbg_ble_gatt(&g);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "complete", g.complete);
    cJSON *sv = cJSON_AddArrayToObject(j, "services");
    char u[BLE_UUID_STR_LEN];
    for (int i = 0; i < g.n_svc; i++) {
        cJSON *o = cJSON_CreateObject();
        uuid_str(&g.svcs[i].uuid, u, sizeof(u));
        cJSON_AddStringToObject(o, "uuid", u);
        cJSON_AddNumberToObject(o, "start", g.svcs[i].start);
        cJSON_AddNumberToObject(o, "end", g.svcs[i].end);
        cJSON_AddItemToArray(sv, o);
    }
    cJSON *ch = cJSON_AddArrayToObject(j, "chars");
    for (int i = 0; i < g.n_chr; i++) {
        const dbg_chr_t *c = &g.chrs[i];
        cJSON *o = cJSON_CreateObject();
        uuid_str(&c->uuid, u, sizeof(u));
        cJSON_AddStringToObject(o, "uuid", u);
        cJSON_AddNumberToObject(o, "svc", c->svc);
        cJSON_AddNumberToObject(o, "def", c->def_handle);
        cJSON_AddNumberToObject(o, "handle", c->val_handle);
        cJSON_AddNumberToObject(o, "cccd", c->cccd);
        cJSON_AddNumberToObject(o, "props", c->props);
        cJSON_AddNumberToObject(o, "sub", c->sub);
        cJSON_AddItemToArray(ch, o);
    }
    return send_json(r, j);
}

// ---- GATT ops --------------------------------------------------------
static esp_err_t h_write(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) { return send_result(r, ESP_ERR_INVALID_ARG); }
    // Sized per request: almost every message is a few bytes, and a static
    // 8 KB buffer cost heap the Wi-Fi stack needs at runtime.
    const char *hx = jstr(j, "hex");
    size_t cap = hx ? strlen(hx) / 2 + 1 : 1;
    uint8_t *buf = cap <= 8192 ? malloc(cap) : NULL;
    int n = buf ? parse_hex(hx, buf, (int)cap) : -1;
    int h = jint(j, "handle", 0);
    esp_err_t e = !buf ? ESP_ERR_NO_MEM : (n <= 0 || h <= 0) ? ESP_ERR_INVALID_ARG
                : dbg_ble_write((uint16_t)h, buf, (size_t)n, jint(j, "gopro", 1), jint(j, "rsp", 1));
    free(buf);
    cJSON_Delete(j);
    return send_result(r, e);
}

static esp_err_t h_read(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    int h = j ? jint(j, "handle", 0) : 0;
    cJSON_Delete(j);
    return send_result(r, h > 0 ? dbg_ble_read((uint16_t)h) : ESP_ERR_INVALID_ARG);
}

static esp_err_t h_subscribe(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    int h = j ? jint(j, "handle", 0) : 0;
    int m = j ? jint(j, "mode", 1) : 1;
    cJSON_Delete(j);
    return send_result(r, h > 0 && m >= 0 && m <= 2 ? dbg_ble_subscribe((uint16_t)h, (uint8_t)m)
                                                    : ESP_ERR_INVALID_ARG);
}

static esp_err_t h_keepalive(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) { return send_result(r, ESP_ERR_INVALID_ARG); }
    dbg_ble_keepalive(jint(j, "on", 1), jint(j, "ms", 3000));
    cJSON_Delete(j);
    return send_result(r, ESP_OK);
}

// ---- GET /api/events?since=N -> chunked JSON ----------------------------
static void json_str_esc(char *dst, size_t cap, const uint8_t *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 7 < cap; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20 || c >= 0x7f) { o += snprintf(dst + o, cap - o, "\\u%04x", c); }
        else { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
}

static esp_err_t h_events(httpd_req_t *r)
{
    char q[48] = {0}, v[16] = {0};
    uint32_t since = 0;
    int max = 80;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) { since = strtoul(v, NULL, 10); }
        if (httpd_query_key_value(q, "max", v, sizeof(v)) == ESP_OK) { max = atoi(v); }
    }
    if (max < 1 || max > 200) { max = 80; }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    static char line[EV_DATA_MAX * 6 + 128];
    uint32_t oldest = evlog_oldest_seq();
    snprintf(line, sizeof(line), "{\"oldest\":%lu,\"ev\":[", (unsigned long)oldest);
    httpd_resp_send_chunk(r, line, HTTPD_RESP_USE_STRLEN);
    static ev_t e;
    uint32_t next = since < oldest ? oldest : since;
    for (int i = 0; i < max && evlog_get(next, &e); i++) {
        int o = snprintf(line, sizeof(line), "%s{\"s\":%lu,\"t\":%lu,\"y\":%u,\"h\":%u,\"st\":%d,\"f\":%u,",
                         i ? "," : "", (unsigned long)e.seq, (unsigned long)e.ms, e.type, e.handle,
                         e.status, e.flags);
        if (e.type == EV_SYS || e.type == EV_ERR) {
            o += snprintf(line + o, sizeof(line) - o, "\"x\":\"");
            json_str_esc(line + o, sizeof(line) - o - 4, e.data, e.len);
            strcat(line, "\"}");
        } else {
            o += snprintf(line + o, sizeof(line) - o, "\"d\":\"");
            hex_into(line + o, e.data, e.len);
            strcat(line, "\"}");
        }
        httpd_resp_send_chunk(r, line, HTTPD_RESP_USE_STRLEN);
        next = e.seq + 1;
    }
    snprintf(line, sizeof(line), "],\"next\":%lu}", (unsigned long)next);
    httpd_resp_send_chunk(r, line, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_log(httpd_req_t *r)
{
    static char buf[4096];      // = SYSLOG_BYTES (evlog.c)
    size_t n = syslog_dump(buf, sizeof(buf));
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_send(r, buf, n);
}

// ---- bonds (shared with the original firmware) ------------------------
static esp_err_t h_bonds(httpd_req_t *r)
{
    uint8_t a[8][6], t[8];
    int n = dbg_ble_bonds(a, t, 8);
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "bonds");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char s[18];
        fmt_addr(s, a[i]);
        cJSON_AddStringToObject(o, "addr", s);
        cJSON_AddNumberToObject(o, "type", t[i]);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(r, j);
}

static esp_err_t h_bond_delete(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) { return send_result(r, ESP_ERR_INVALID_ARG); }
    esp_err_t e;
    uint8_t a[6];
    if (jint(j, "all", 0)) { e = dbg_ble_delete_all_bonds(); }
    else if (parse_addr(jstr(j, "addr"), a)) { e = dbg_ble_delete_bond(a, (uint8_t)jint(j, "type", 0)); }
    else { e = ESP_ERR_INVALID_ARG; }
    cJSON_Delete(j);
    return send_result(r, e);
}

// ---- firmware: OTA upload / boot the other slot / reboot ------------------
static void reboot_soon(httpd_req_t *r, const char *body)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Clear-Site-Data", "\"cache\"");
    httpd_resp_sendstr(r, body);
    vTaskDelay(pdMS_TO_TICKS(1500));     // let the reply drain over the SoftAP
    esp_restart();
}

#define OTA_FAIL(code, msg) do { \
        if (h) { esp_ota_abort(h); } \
        httpd_resp_set_status(r, code); \
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"" msg "\"}"); \
        return ESP_OK; } while (0)

// Same wire contract as the original's /api/ota: raw .bin body (a multipart
// wrapper is tolerated by skipping to the 0xE9 image magic).
static esp_err_t h_ota(httpd_req_t *r)
{
    esp_ota_handle_t h = 0;
    int total = r->content_len;
    if (total < 65536) { OTA_FAIL("400 Bad Request", "too small -- upload an app .bin"); }
    dbg_ble_disconnect();
    dbg_ble_scan(false, 0, false);
    vTaskDelay(pdMS_TO_TICKS(300));

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { OTA_FAIL("500 Internal Server Error", "no OTA partition"); }
    if ((size_t)total > part->size + 4096) { OTA_FAIL("400 Bad Request", "image larger than the OTA slot"); }
    ESP_LOGW(TAG, "OTA -> %s, %d B", part->label, total);
    if (esp_ota_begin(part, (size_t)total < part->size ? (size_t)total : part->size, &h) != ESP_OK) {
        h = 0;
        OTA_FAIL("500 Internal Server Error", "ota_begin");
    }

    #define OTA_BUF 8192
    static uint8_t buf[OTA_BUF];
    int remaining = total, timeouts = 0;
    bool started = false;
    size_t written = 0, fill = 0;
    while (remaining > 0 || fill > 0) {
        while (fill < OTA_BUF && remaining > 0) {
            int want = (int)(OTA_BUF - fill);
            if (want > remaining) { want = remaining; }
            int n = httpd_req_recv(r, (char *)buf + fill, want);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 15) { OTA_FAIL("408 Request Timeout", "upload stalled"); }
                break;
            }
            if (n <= 0) { OTA_FAIL("400 Bad Request", "recv"); }
            timeouts = 0;
            fill += n;
            remaining -= n;
        }
        uint8_t *p = buf;
        size_t m = fill;
        if (!started && m) {
            uint8_t *e9 = memchr(buf, 0xE9, m);
            if (!e9) { fill = 0; continue; }
            started = true;
            m -= (size_t)(e9 - buf);
            p = e9;
        }
        // A multipart trailer after the image would be rejected by
        // esp_ota_end's checksum anyway; never write past the slot.
        if (written + m > part->size) { m = part->size - written; }
        if (m && esp_ota_write(h, p, m) != ESP_OK) { OTA_FAIL("500 Internal Server Error", "flash write"); }
        written += m;
        fill = 0;
    }
    if (!started || written < 65536) { OTA_FAIL("400 Bad Request", "no valid image found"); }
    esp_err_t e = esp_ota_end(h);
    h = 0;
    if (e != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"image rejected: %s\"}", esp_err_to_name(e));
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, msg);
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) { OTA_FAIL("500 Internal Server Error", "set_boot_partition"); }
    esp_app_desc_t d = {0};
    esp_ota_get_partition_description(part, &d);
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":true,\"rebooting\":true,\"project\":\"%s\",\"version\":\"%s\"}",
             d.project_name, d.version);
    ESP_LOGW(TAG, "OTA ok: %s %s (%u B) -> rebooting", d.project_name, d.version, (unsigned)written);
    reboot_soon(r, body);
    return ESP_OK;
}

// Boot whatever is in the other OTA slot -- normally the original RecLink
// firmware this debug image was uploaded from. No upload needed.
static esp_err_t h_boot_other(httpd_req_t *r)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"other slot holds no valid app\"}");
    }
    esp_err_t e = esp_ota_set_boot_partition(other);
    if (e != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(e));
        return httpd_resp_sendstr(r, msg);
    }
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":true,\"rebooting\":true,\"project\":\"%s\",\"version\":\"%s\"}",
             d.project_name, d.version);
    ESP_LOGW(TAG, "boot other slot %s: %s %s", other->label, d.project_name, d.version);
    reboot_soon(r, body);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *r) { reboot_soon(r, "{\"ok\":true,\"rebooting\":true}"); return ESP_OK; }

// ---- static assets + captive probes -----------------------------------
static esp_err_t serve_gz(httpd_req_t *r, const uint8_t *s, const uint8_t *e, const char *ct)
{
    httpd_resp_set_type(r, ct);
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, (const char *)s, e - s);
}

static esp_err_t h_root(httpd_req_t *r)
{
    const char *u = r->uri;
    if (!strncmp(u, "/app.js", 7))     { return serve_gz(r, app_js_start, app_js_end, "application/javascript"); }
    if (!strncmp(u, "/ops.js", 7))     { return serve_gz(r, ops_js_start, ops_js_end, "application/javascript"); }
    if (!strncmp(u, "/catalog.js", 11)) { return serve_gz(r, catalog_js_start, catalog_js_end, "application/javascript"); }
    if (!strncmp(u, "/style.css", 10)) { return serve_gz(r, style_css_start, style_css_end, "text/css"); }
    // OS connectivity probes: answer "online" so no captive sheet pops up.
    if (strstr(u, "generate_204") || strstr(u, "gen_204")) {
        httpd_resp_set_status(r, "204 No Content");
        return httpd_resp_send(r, NULL, 0);
    }
    if (strstr(u, "connecttest.txt") || strstr(u, "ncsi.txt")) {
        httpd_resp_set_type(r, "text/plain");
        return httpd_resp_sendstr(r, "Microsoft Connect Test");
    }
    if (strstr(u, "hotspot-detect") || strstr(u, "library/test") || strstr(u, "/success")) {
        httpd_resp_set_type(r, "text/html");
        return httpd_resp_sendstr(r, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    }
    return serve_gz(r, index_html_start, index_html_end, "text/html");
}

esp_err_t web_start(void)
{
    httpd_handle_t s = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 32;
    hc.lru_purge_enable = true;
    hc.stack_size = 8192;
    hc.recv_wait_timeout = 20;
    hc.send_wait_timeout = 20;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t e = httpd_start(&s, &hc);
    if (e != ESP_OK) { return e; }
    const httpd_uri_t routes[] = {
        { "/api/status",       HTTP_GET,  h_status,      NULL },
        { "/api/scan",         HTTP_POST, h_scan,        NULL },
        { "/api/devices",      HTTP_GET,  h_devices,     NULL },
        { "/api/connect",      HTTP_POST, h_connect,     NULL },
        { "/api/disconnect",   HTTP_POST, h_disconnect,  NULL },
        { "/api/secure",       HTTP_POST, h_secure,      NULL },
        { "/api/auto",         HTTP_POST, h_auto,        NULL },
        { "/api/discover",     HTTP_POST, h_discover,    NULL },
        { "/api/gatt",         HTTP_GET,  h_gatt,        NULL },
        { "/api/mtu",          HTTP_POST, h_mtu,         NULL },
        { "/api/subscribe",    HTTP_POST, h_subscribe,   NULL },
        { "/api/subscribe_all", HTTP_POST, h_sub_all,    NULL },
        { "/api/write",        HTTP_POST, h_write,       NULL },
        { "/api/read",         HTTP_POST, h_read,        NULL },
        { "/api/keepalive",    HTTP_POST, h_keepalive,   NULL },
        { "/api/events",       HTTP_GET,  h_events,      NULL },
        { "/api/log",          HTTP_GET,  h_log,         NULL },
        { "/api/bonds",        HTTP_GET,  h_bonds,       NULL },
        { "/api/bonds/delete", HTTP_POST, h_bond_delete, NULL },
        { "/api/ota",          HTTP_POST, h_ota,         NULL },
        { "/api/boot_other",   HTTP_POST, h_boot_other,  NULL },
        { "/api/reboot",       HTTP_POST, h_reboot,      NULL },
        { "/*",                HTTP_GET,  h_root,        NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s, &routes[i]);
    }
    return ESP_OK;
}
