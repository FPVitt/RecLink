// http_api.c -- REST surface (spec 10.4). All POSTs require !armed (409).
// No auth beyond WPA2 -- documented bench-only limitation (spec 10.4).
#include "portal_internal.h"
#include "portal.h"

#include <string.h>
#include <stdlib.h>

#include "cfg.h"
#include "fc_state.h"
#include "cam_state.h"
#include "camera.h"
#include "ble_central.h"
#include "control.h"
#include "msp.h"
#include "platform.h"
#include "applog.h"
#include "monoclock.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "api";

// embedded gzip assets
extern const uint8_t idx_start[] asm("_binary_index_html_gz_start");
extern const uint8_t idx_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t js_start[]  asm("_binary_app_js_gz_start");
extern const uint8_t js_end[]    asm("_binary_app_js_gz_end");
extern const uint8_t css_start[] asm("_binary_style_css_gz_start");
extern const uint8_t css_end[]   asm("_binary_style_css_gz_end");

// ---- helpers ------------------------------------------------------
static bool fc_armed(void)
{
    fc_state_t s; fc_state_get(&s);
    return s.armed;
}

static esp_err_t send_json(httpd_req_t *r, cJSON *j, const char *status)
{
    portal_note_client_activity();
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    if (status) { httpd_resp_set_status(r, status); }
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t reject_if_armed(httpd_req_t *r)
{
    if (fc_armed()) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"armed\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *read_body(httpd_req_t *r, size_t max)
{
    size_t len = r->content_len;
    if (len == 0 || len > max) { return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { return NULL; }
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = '\0';
    return buf;
}

// ---- GET /api/status --------------------------------------------
static esp_err_t h_status(httpd_req_t *r)
{
    fc_state_t fc;   fc_state_get(&fc);
    cam_state_t cam; cam_state_get(&cam);
    const cfg_t *cfg = cfg_get();

    cJSON *j = cJSON_CreateObject();
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(j, "fw", ad->version);
    cJSON_AddNumberToObject(j, "uptime_s", now_ms() / 1000);

    cJSON *flags = cJSON_AddArrayToObject(j, "flags");
    uint32_t fl = bridge_flags_all();
    const char *names[] = { "CFG_RESET","REPAIR_NEEDED","OSD_UNSUPPORTED","FC_OFFLINE","CLOCK_SYNC_FAIL","WDT_RECOVERED" };
    for (int i = 0; i < 6; i++) { if (fl & (1u << i)) { cJSON_AddItemToArray(flags, cJSON_CreateString(names[i])); } }

    cJSON *m = cJSON_AddObjectToObject(j, "msp");
    cJSON_AddBoolToObject(m, "connected", fc.link_up);
    char api[8]; snprintf(api, sizeof(api), "%u.%u", fc.api_major, fc.api_minor);
    cJSON_AddStringToObject(m, "api", api);
    cJSON_AddStringToObject(m, "variant", fc.fc_variant);
    cJSON_AddBoolToObject(m, "armed", fc.armed);
    char canv[12]; snprintf(canv, sizeof(canv), "%ux%u", fc.osd_cols, fc.osd_rows);
    cJSON_AddStringToObject(m, "canvas", canv);
    cJSON *aux = cJSON_AddArrayToObject(m, "aux");
    for (int i = 0; i < fc.rc_channel_count; i++) { cJSON_AddItemToArray(aux, cJSON_CreateNumber(fc.rc[i])); }
    cJSON *gps = cJSON_AddObjectToObject(m, "gps");
    cJSON_AddBoolToObject(gps, "present", fc.gps_present);
    cJSON_AddNumberToObject(gps, "fix", fc.gps.fix);
    cJSON_AddNumberToObject(gps, "sats", fc.gps.sats);
    cJSON_AddBoolToObject(gps, "good", fc.gps_good);
    int64_t fc_utc = 0; fc_now_utc_ms(&fc_utc, 0);
    cJSON_AddNumberToObject(m, "fc_utc", (double)(fc_utc / 1000));
    cJSON_AddNumberToObject(m, "last_rx_ms", (double)(now_ms() - fc.last_rx_ms));

    // MSP diagnostics: measured receive cadence + arm-detection internals.
    cJSON *dbg = cJSON_AddObjectToObject(m, "debug");
    cJSON_AddNumberToObject(dbg, "rc_period_ms", fc.rc_period_ms);
    cJSON_AddNumberToObject(dbg, "rc_hz",
        fc.rc_period_ms ? (double)((int)(10000.0 / fc.rc_period_ms + 0.5)) / 10.0 : 0);
    cJSON_AddNumberToObject(dbg, "rc_age_ms",
        fc.rc_last_rx_ms ? (double)(now_ms() - fc.rc_last_rx_ms) : -1);
    cJSON_AddNumberToObject(dbg, "rc_channels", fc.rc_channel_count);
    cJSON_AddNumberToObject(dbg, "status_period_ms", fc.status_period_ms);
    cJSON_AddNumberToObject(dbg, "status_hz",
        fc.status_period_ms ? (double)((int)(10000.0 / fc.status_period_ms + 0.5)) / 10.0 : 0);
    cJSON_AddNumberToObject(dbg, "rc_nominal_hz", 10);       // msp_sched MSP_RC period
    cJSON_AddNumberToObject(dbg, "status_nominal_hz", 20);   // msp_sched MSP_STATUS period
    cJSON_AddNumberToObject(dbg, "arm_bit", fc.arm_bit_index);
    cJSON_AddBoolToObject(dbg, "arm_bit_resolved", fc.arm_bit_index >= 0);
    cJSON_AddNumberToObject(dbg, "arm_disable_flags", (double)fc.arming_disable_flags);
    cJSON_AddBoolToObject(dbg, "armed_raw", fc.armed);
    // Boot-time timing (ms since this ESP boot, not since cfg load/reconnect):
    // 0 = hasn't happened yet this boot.
    cJSON_AddNumberToObject(dbg, "link_up_first_ms", fc.link_up_first_ms);
    cJSON_AddNumberToObject(dbg, "osd_first_ms", fc.osd_first_ms);

    cJSON *clk = cJSON_AddObjectToObject(j, "clock");
    bool readable, last_ok, ever; int32_t delta_ms; uint32_t last_res;
    ctrl_clock_status(&readable, &delta_ms, &last_ok, &last_res, &ever);
    if (strcmp(cfg->camera.brand, "dji") == 0) { readable = false; }
    cJSON_AddBoolToObject(clk, "enabled", cfg->clock.enabled);
    cJSON_AddBoolToObject(clk, "readable", readable);
    if (cam.cam_utc_ms.valid) { cJSON_AddNumberToObject(clk, "cam_utc", (double)(cam.cam_utc_ms.v / 1000)); }
    cJSON_AddNumberToObject(clk, "delta_s", delta_ms / 1000);
    if (ever) {
        cJSON *ls = cJSON_AddObjectToObject(clk, "last_sync");
        cJSON_AddNumberToObject(ls, "age_s", (now_ms() - last_res) / 1000);
        cJSON_AddStringToObject(ls, "result", last_ok ? "OK" : "FAILED");
    }

    cJSON *c = cJSON_AddObjectToObject(j, "camera");
    cJSON_AddStringToObject(c, "brand", cfg->camera.brand);
    const char *ls_names[] = { "IDLE","SCAN","CONNECTING","BONDING","SYNCING","READY","LOST" };
    cJSON_AddStringToObject(c, "link", ls_names[cam.link <= CAM_LINK_LOST ? cam.link : 0]);
    cJSON_AddStringToObject(c, "name", cam.name);
    char maddr[20]; snprintf(maddr, sizeof(maddr), "%02X:%02X:%02X:%02X:%02X:%02X",
                             cam.addr[0],cam.addr[1],cam.addr[2],cam.addr[3],cam.addr[4],cam.addr[5]);
    cJSON_AddStringToObject(c, "addr", maddr);
    cJSON_AddNumberToObject(c, "rssi", cam.rssi);
    cJSON_AddBoolToObject(c, "bonded", cfg->camera.bonded);
    cJSON_AddNumberToObject(c, "last_rx_ms", (double)(now_ms() - cam.last_rx_ms));

    // All bonded cameras (spec 7.1). `active` = the one we connect to / just
    // connected; `connected` = link is up right now.
    bool link_up_now = (cam.link == CAM_LINK_READY);
    cJSON_AddNumberToObject(c, "count", cfg_cam_count());
    cJSON *cams = cJSON_AddArrayToObject(c, "cameras");
    for (int i = 0; i < CAM_MAX_BONDS; i++) {
        cam_bond_t b; uint8_t poll_ds = 0;
        if (!cfg_cam_at(i, &b, &poll_ds)) { continue; }
        cJSON *ce = cJSON_CreateObject();
        cJSON_AddStringToObject(ce, "brand", b.brand);
        cJSON_AddStringToObject(ce, "name", b.name);
        char bad[20]; snprintf(bad, sizeof(bad), "%02X:%02X:%02X:%02X:%02X:%02X",
                               b.addr[0],b.addr[1],b.addr[2],b.addr[3],b.addr[4],b.addr[5]);
        cJSON_AddStringToObject(ce, "addr", bad);
        bool is_active = memcmp(b.addr, cfg->camera.addr, 6) == 0 && cfg->camera.bonded;
        cJSON_AddBoolToObject(ce, "active", is_active);
        cJSON_AddBoolToObject(ce, "connected", is_active && link_up_now);
        cJSON_AddNumberToObject(ce, "poll_ds", poll_ds);   // deciseconds
        cJSON_AddItemToArray(cams, ce);
    }

    // Cameras found during pairing discovery (empty otherwise).
    cam_found_t fcams[6];
    int fn = cam_found_list(fcams, 6);
    if (fn) {
        cJSON *fa = cJSON_AddArrayToObject(c, "found");
        for (int i = 0; i < fn; i++) {
            cJSON *fe = cJSON_CreateObject();
            cJSON_AddStringToObject(fe, "name", fcams[i].name);
            char fad[20]; snprintf(fad, sizeof(fad), "%02X:%02X:%02X:%02X:%02X:%02X",
                                   fcams[i].addr[0],fcams[i].addr[1],fcams[i].addr[2],fcams[i].addr[3],fcams[i].addr[4],fcams[i].addr[5]);
            cJSON_AddStringToObject(fe, "addr", fad);
            cJSON_AddNumberToObject(fe, "rssi", fcams[i].rssi);
            cJSON_AddItemToArray(fa, fe);
        }
    }

    // Record-trigger diagnostics (spec 8.1/8.2): what the switch/button decode
    // currently sees and intends, independent of whether the camera has caught
    // up yet. Lets a "why won't it stop" report be settled from the portal.
    cJSON *rd = cJSON_AddObjectToObject(j, "record");
    bool r_raw = false, r_sw = false, r_desired = false; uint8_t r_phase = 0, r_cmd = 0;
    ctrl_record_status(&r_raw, &r_sw, &r_desired, &r_phase, &r_cmd);
    static const char *phase_names[] = {
        "idle", "start_delay", "verify_start", "stop_delay", "verify_stop" };
    static const char *cmd_names[] = { "none", "stop", "start" };   // tribool_t order
    cJSON_AddNumberToObject(rd, "mode", cfg->record.mode);
    cJSON_AddNumberToObject(rd, "sw_mode", cfg->record.sw.mode);   // 0 switch, 1 button
    cJSON_AddBoolToObject(rd, "raw_active", r_raw);       // AUX channel vs threshold, right now
    cJSON_AddBoolToObject(rd, "sw_active", r_sw);         // after switch/button decode (latch in button mode)
    cJSON_AddBoolToObject(rd, "desired", r_desired);      // what the trigger wants recording to be
    cJSON_AddStringToObject(rd, "phase", phase_names[r_phase <= 4 ? r_phase : 0]);
    cJSON_AddStringToObject(rd, "last_cmd", cmd_names[r_cmd <= 2 ? r_cmd : 0]);  // last START/STOP we sent

    // BLE link diagnostics (Status tab).
    cJSON *bd = cJSON_AddObjectToObject(j, "ble");
    cJSON_AddStringToObject(bd, "link", ls_names[cam.link <= CAM_LINK_LOST ? cam.link : 0]);
    cJSON_AddNumberToObject(bd, "state_age_s", cam_link_age_ms() / 1000);
    cJSON_AddStringToObject(bd, "scan", cam_scan_state());
    if (cam.link == CAM_LINK_READY) {
        cJSON_AddNumberToObject(bd, "rssi", cam.rssi);
        cJSON_AddNumberToObject(bd, "last_rx_ms", (double)(now_ms() - cam.last_rx_ms));
        cJSON_AddNumberToObject(bd, "mtu", ble_central_mtu());
        cJSON_AddNumberToObject(bd, "poll_ms", cfg_active_poll_ms());
    }
    cJSON_AddNumberToObject(bd, "cfg_bonds", cfg_cam_count());
    cJSON_AddNumberToObject(bd, "stack_bonds", ble_central_bond_count());
    cJSON_AddNumberToObject(bd, "notify_drops", (double)cam_notify_drops());
    cJSON_AddBoolToObject(bd, "repair_needed", (bridge_flags_all() & FLAG_REPAIR_NEEDED) != 0);

    cJSON *cs = cJSON_AddObjectToObject(j, "cam_state");
    #define VU(name, field) do { cJSON *o = cJSON_AddObjectToObject(cs, name); \
        cJSON_AddNumberToObject(o, "v", cam.field.v); cJSON_AddBoolToObject(o, "valid", cam.field.valid); } while (0)
    cJSON_AddStringToObject(cs, "recording",
        cam.recording == TRIBOOL_TRUE ? "true" : cam.recording == TRIBOOL_FALSE ? "false" : "unknown");
    if (cam.rec_elapsed_s.valid) { cJSON_AddNumberToObject(cs, "elapsed_s", cam.rec_elapsed_s.v); }
    VU("battery_pct", battery_pct);
    VU("sd_remaining_kb", sd_remaining_kb);
    VU("rec_remaining_s", rec_remaining_s);
    { cJSON *o = cJSON_AddObjectToObject(cs, "resolution");
      cJSON_AddStringToObject(o, "v", cam.resolution.v); cJSON_AddBoolToObject(o, "valid", cam.resolution.valid); }
    { cJSON *o = cJSON_AddObjectToObject(cs, "fps");
      cJSON_AddStringToObject(o, "v", cam.framerate.v); cJSON_AddBoolToObject(o, "valid", cam.framerate.valid); }
    { cJSON *o = cJSON_AddObjectToObject(cs, "aspect");
      cJSON_AddStringToObject(o, "v", cam.aspect.v); cJSON_AddBoolToObject(o, "valid", cam.aspect.valid); }
    { cJSON *o = cJSON_AddObjectToObject(cs, "profile");
      cJSON_AddStringToObject(o, "v", cam.profile.v); cJSON_AddBoolToObject(o, "valid", cam.profile.valid); }
    { cJSON *o = cJSON_AddObjectToObject(cs, "mode");
      cJSON_AddStringToObject(o, "v", cam.mode_name.v); cJSON_AddBoolToObject(o, "valid", cam.mode_name.valid); }
    VU("bit_depth", bit_depth);
    cJSON_AddBoolToObject(cs, "sd_missing", cam.sd_missing);
    cJSON_AddBoolToObject(cs, "sd_full", cam.sd_full);
    cJSON_AddBoolToObject(cs, "hot", cam.hot);
    #undef VU

    cJSON *wifi = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddBoolToObject(wifi, "portal", portal_active());
    cJSON_AddBoolToObject(wifi, "default_pw", strcmp(cfg->wifi.password, CFG_DEFAULT_WIFI_PW) == 0);
    cJSON_AddBoolToObject(wifi, "pw_nag_off", (cfg->ui_flags & 2) != 0);

    cJSON *errs = cJSON_AddArrayToObject(j, "errors");
    cfg_stats_t st; cfg_stats_load(&st);
    char eb[48];
    snprintf(eb, sizeof(eb), "reconnects:%lu", (unsigned long)st.reconnects);
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    snprintf(eb, sizeof(eb), "msp_crc:%lu", (unsigned long)st.msp_crc_errors);
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    crash_stats_t cr = rtcnv_get();
    snprintf(eb, sizeof(eb), "wdt_recov:%lu", (unsigned long)cr.wdt_recoveries);
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    snprintf(eb, sizeof(eb), "ble_notify_drops:%lu", (unsigned long)cam_notify_drops());
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    snprintf(eb, sizeof(eb), "heap_free:%lu", (unsigned long)esp_get_free_heap_size());
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    snprintf(eb, sizeof(eb), "heap_min:%lu", (unsigned long)esp_get_minimum_free_heap_size());
    cJSON_AddItemToArray(errs, cJSON_CreateString(eb));
    if (cr.last_disconnect[0]) { cJSON_AddItemToArray(errs, cJSON_CreateString(cr.last_disconnect)); }

    return send_json(r, j, NULL);
}

// ---- GET/POST /api/config --------------------------------------
static esp_err_t h_config_get(httpd_req_t *r)
{
    return send_json(r, cfg_to_json(cfg_get()), NULL);
}

static esp_err_t h_config_post(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }

    char q[32] = {0};
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && strstr(q, "factory=1")) {
        cfg_factory_erase();
        cam_erase_all_bonds();
        httpd_resp_sendstr(r, "{\"ok\":true,\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return ESP_OK;
    }

    char *body = read_body(r, 8192);
    if (!body) { httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, "{\"ok\":false}"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"bad json\"}"); return ESP_OK; }

    cfg_t cand; cfg_snapshot(&cand);
    char err[80];
    json_merge_cfg(j, &cand, err, sizeof(err));
    cJSON_Delete(j);

    esp_err_t e = cfg_validate(&cand, err, sizeof(err));
    if (e != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON *ea = cJSON_AddArrayToObject(resp, "errors");
        cJSON_AddItemToArray(ea, cJSON_CreateString(err));
        return send_json(r, resp, "400 Bad Request");
    }
    e = cfg_commit(&cand);      // synchronous NVS commit (spec 11.0)

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    cJSON_AddBoolToObject(resp, "applied", e == ESP_OK);
    return send_json(r, resp, e == ESP_OK ? NULL : "500 Internal Server Error");
}

// ---- camera ---------------------------------------------------
static esp_err_t h_cam_pair(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 256);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *br = j ? cJSON_GetObjectItem(j, "brand") : NULL;
    const cJSON *ad = j ? cJSON_GetObjectItem(j, "addr") : NULL;   // "AA:BB:CC:DD:EE:FF" -> bond to it
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(br)) {
        uint8_t a[6];
        if (cJSON_IsString(ad) && sscanf(ad->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                                         &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            e = cam_pair_select(br->valuestring, a);
        } else {
            e = cam_pair_begin(br->valuestring);
        }
    }
    cJSON_Delete(j);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    return send_json(r, resp, e == ESP_OK ? NULL : "400 Bad Request");
}

static esp_err_t h_cam_forget(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    // Optional {"addr":"AA:BB:.."} -> forget just that camera; no body -> forget all.
    char *body = read_body(r, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ad = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    esp_err_t e;
    uint8_t a[6];
    if (cJSON_IsString(ad) && sscanf(ad->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                                     &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        e = cam_forget_one(a);
    } else {
        e = cam_forget();
    }
    cJSON_Delete(j);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    return send_json(r, resp, NULL);
}

static esp_err_t h_cam_refresh(httpd_req_t *r)
{
    cam_refresh();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(r, resp, NULL);
}

// {"addr":"AA:BB:..","poll_ds":N} -> per-camera BLE telemetry poll interval (deciseconds).
static esp_err_t h_cam_poll(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ad = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    const cJSON *ps = j ? cJSON_GetObjectItem(j, "poll_ds") : NULL;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    uint8_t a[6];
    if (cJSON_IsString(ad) && cJSON_IsNumber(ps) &&
        sscanf(ad->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        int v = (int)ps->valuedouble;
        if (v < CAM_POLL_DS_MIN) { v = CAM_POLL_DS_MIN; }
        if (v > CAM_POLL_DS_MAX) { v = CAM_POLL_DS_MAX; }
        e = cfg_cam_set_poll(a, (uint8_t)v);
    }
    cJSON_Delete(j);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    return send_json(r, resp, e == ESP_OK ? NULL : "400 Bad Request");
}

static esp_err_t h_cam_preset(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *s = j ? cJSON_GetObjectItem(j, "slot") : NULL;
    int slot = cJSON_IsNumber(s) ? (int)s->valuedouble : -1;
    cJSON_Delete(j);

    cJSON *resp = cJSON_CreateObject();
    if (slot < 0 || slot > 2) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "slot must be 0..2");
        return send_json(r, resp, "400 Bad Request");
    }
    cam_submit(CAM_OP_SELECT_PRESET, slot);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(r, resp, NULL);
}

static void test_stop_cb(void *arg) { (void)arg; cam_submit(CAM_OP_RECORD_STOP, 0); }
static esp_err_t h_cam_test(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    cam_submit(CAM_OP_RECORD_START, 0);
    const esp_timer_create_args_t ta = { .callback = test_stop_cb, .name = "camtest" };
    esp_timer_handle_t th;
    if (esp_timer_create(&ta, &th) == ESP_OK) { esp_timer_start_once(th, 3000000); }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(r, resp, NULL);
}

// ---- clock sync ---------------------------------------------
static esp_err_t h_clock_sync(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    cam_state_t cam; cam_state_get(&cam);
    if (cam.recording == TRIBOOL_TRUE) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"recording\"}");
        return ESP_OK;
    }
    char why[64];
    if (!ctrl_clock_gate_check(why, sizeof(why))) {
        httpd_resp_set_status(r, "412 Precondition Failed");
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "error", why);
        return send_json(r, e, NULL);
    }
    char reason[64];
    ctrl_clock_force(reason, sizeof(reason));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "action", "queued");
    // delta from the *previous* completed sync, for context (0 if none / DJI blind).
    cJSON_AddNumberToObject(resp, "last_delta_ms", ctrl_clock_last_delta_ms());
    return send_json(r, resp, NULL);
}

// ---- misc ---------------------------------------------------
static esp_err_t h_identify(httpd_req_t *r)
{
    led_identify(3000);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

// Acknowledge the WDT_RECOVERED sticky flag (spec 3.4 flag table).
static esp_err_t h_flags_ack(httpd_req_t *r)
{
    rtcnv_note_wdt_recovered_ack();
    bridge_flag_clear(FLAG_WDT_RECOVERED);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    httpd_resp_sendstr(r, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_logs(httpd_req_t *r)
{
    portal_note_client_activity();
    static char buf[APPLOG_RING_BYTES + 1];
    size_t n = applog_dump(buf, sizeof(buf));
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_msp_scan(httpd_req_t *r)
{
    fc_state_t fc; fc_state_get(&fc);
    cJSON *j = cJSON_CreateObject();
    cJSON *aux = cJSON_AddArrayToObject(j, "aux");
    for (int i = 4; i < fc.rc_channel_count; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "aux", i - 3);
        cJSON_AddNumberToObject(e, "us", fc.rc[i]);
        cJSON_AddItemToArray(aux, e);
    }
    return send_json(r, j, NULL);
}

#define OTA_FAIL(code, msg) do { \
        ota_stream_abort(); \
        httpd_resp_set_status(r, code); \
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"" msg "\"}"); \
        return ESP_OK; } while (0)

static esp_err_t h_ota(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char q[24] = {0};
    bool erase = (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) && strstr(q, "erase=1");
    int total = r->content_len;
    ESP_LOGW("api", "OTA POST: content-len %d, free heap %u", total,
             (unsigned)esp_get_free_heap_size());
    if (total < 65536) {
        httpd_resp_set_status(r, "400 Bad Request");
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"too small -- upload build/fc_cam_bridge.bin\"}");
        return ESP_OK;
    }
    // Free the single core for Wi-Fi + flash: drop the BLE link and disable
    // Wi-Fi power-save for the duration (reboot restores both).
    cam_ble_suspend();
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(300));

    if (ota_stream_begin((size_t)total) != ESP_OK) { OTA_FAIL("500 Internal Server Error", "ota_begin"); }

    // Accumulate many small socket reads into a big buffer and flash it in one
    // shot. This keeps httpd_req_recv() being called back-to-back so the socket
    // is drained and the TCP window stays open, instead of stalling for the
    // duration of every tiny flash write (which collapsed the transfer at ~58%).
    // Static, not malloc'd: heap is tight during the portal and this must not
    // fail. 16 KB .bss, only meaningful on a build with the portal.
    #define OTA_BUF 16384
    static uint8_t buf[OTA_BUF];

    int  remaining = total;
    bool started = false;
    size_t written = 0, fill = 0;
    int  timeouts = 0;
    uint32_t last_log = 0;

    while (remaining > 0 || fill > 0) {
        while (fill < OTA_BUF && remaining > 0) {
            int want = (int)(OTA_BUF - fill);
            if (want > remaining) { want = remaining; }
            int n = httpd_req_recv(r, (char *)buf + fill, want);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 15) { ESP_LOGE("api", "OTA recv stalled at %u B", (unsigned)written); OTA_FAIL("408 Request Timeout", "upload stalled"); }
                break;                       // flush what we have, then retry
            }
            if (n <= 0) { ESP_LOGE("api", "OTA recv=%d at %u B", n, (unsigned)written); OTA_FAIL("400 Bad Request", "recv"); }
            timeouts = 0;
            fill += n;
            remaining -= n;
        }

        uint8_t *p = buf;
        size_t   m = fill;
        if (!started && m) {
            uint8_t *e9 = memchr(buf, 0xE9, m);   // skip a multipart wrapper
            if (!e9) { fill = 0; continue; }
            started = true;
            m -= (size_t)(e9 - buf);
            p = e9;
        }
        if (m && ota_stream_write(p, m) != ESP_OK) { OTA_FAIL("500 Internal Server Error", "flash write"); }
        written += m;
        fill = 0;

        if (now_ms() - last_log > 1000) {
            last_log = now_ms();
            ESP_LOGI("api", "OTA %u / %d B (%d%%)", (unsigned)written, total, (int)(100LL * written / total));
        }
    }
    ESP_LOGW("api", "OTA received %u B, finalising", (unsigned)written);
    if (!started || written < 65536) { OTA_FAIL("400 Bad Request", "no valid image found"); }

    esp_err_t e = ota_stream_finish();
    if (e != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(e));
        httpd_resp_set_status(r, "500 Internal Server Error");
        httpd_resp_sendstr(r, msg);
        return ESP_OK;
    }
    if (erase) {
        // Wipe cfg + BLE bonds so the new firmware boots on factory defaults.
        cfg_factory_erase();
        cam_erase_all_bonds();
        ESP_LOGW("api", "OTA: settings + bonds erased -> factory defaults");
    }
    ESP_LOGW("api", "OTA ok (%u bytes) -- rebooting", (unsigned)written);
    // Tell the browser to drop its cached copy of the portal -- the new firmware
    // ships new index.html/app.js/style.css and the client will hard-reload once
    // the device is back up. Chromium/Firefox honour this; Safari ignores it, so
    // the client also reloads with a cache-busting query (see app.js).
    httpd_resp_set_hdr(r, "Clear-Site-Data", "\"cache\"");
    httpd_resp_sendstr(r, erase ? "{\"ok\":true,\"rebooting\":true,\"erased\":true}"
                                : "{\"ok\":true,\"rebooting\":true}");
    // Give the TCP stack time to flush the response over the (slow) SoftAP link
    // before the reboot kills the socket -- else the client's fetch rejects and
    // the UI shows "failed" for a successful update.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// ---- static assets + captive fallback -----------------------
static esp_err_t serve_gz(httpd_req_t *r, const uint8_t *s, const uint8_t *e, const char *ct)
{
    httpd_resp_set_type(r, ct);
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");   // bench tool; always show the flashed UI
    return httpd_resp_send(r, (const char *)s, e - s);
}

// OS connectivity-check probes. Answer them with what each OS expects so it
// concludes "internet is fine" and does NOT force the captive-portal login
// sheet. The user opens 10.0.0.1 themselves when they want the config page.
static bool answer_captive_probe(httpd_req_t *r, const char *u)
{
    char host[40] = {0};
    httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host));

    if (strstr(u, "generate_204") || strstr(u, "gen_204")) {          // Android
        httpd_resp_set_status(r, "204 No Content");
        httpd_resp_send(r, NULL, 0);
        return true;
    }
    if (strstr(u, "connecttest.txt") || strstr(u, "ncsi.txt")) {      // Windows
        httpd_resp_set_type(r, "text/plain");
        httpd_resp_sendstr(r, "Microsoft Connect Test");
        return true;
    }
    if (strstr(host, "captive.apple.com") || strstr(u, "hotspot-detect") ||
        strstr(u, "/success") || strstr(u, "library/test")) {         // Apple
        httpd_resp_set_type(r, "text/html");
        httpd_resp_sendstr(r,
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        return true;
    }
    return false;
}

static esp_err_t h_root(httpd_req_t *r)
{
    portal_note_client_activity();
    const char *u = r->uri;
    if (strcmp(u, "/app.js") == 0)    { return serve_gz(r, js_start, js_end, "application/javascript"); }
    if (strcmp(u, "/style.css") == 0) { return serve_gz(r, css_start, css_end, "text/css"); }
    if (answer_captive_probe(r, u))   { return ESP_OK; }
    // A real request for the UI (or any other path) -> the page.
    return serve_gz(r, idx_start, idx_end, "text/html");
}

// ---- registration -----------------------------------------
void http_api_register(httpd_handle_t s)
{
    const httpd_uri_t routes[] = {
        { "/api/status",        HTTP_GET,  h_status,      NULL },
        { "/api/config",        HTTP_GET,  h_config_get,  NULL },
        { "/api/config",        HTTP_POST, h_config_post, NULL },
        { "/api/camera/pair",   HTTP_POST, h_cam_pair,    NULL },
        { "/api/camera/forget", HTTP_POST, h_cam_forget,  NULL },
        { "/api/camera/refresh", HTTP_POST, h_cam_refresh, NULL },
        { "/api/camera/poll",   HTTP_POST, h_cam_poll,    NULL },
        { "/api/camera/preset", HTTP_POST, h_cam_preset,  NULL },
        { "/api/camera/test",   HTTP_POST, h_cam_test,    NULL },
        { "/api/clock/sync",    HTTP_POST, h_clock_sync,  NULL },
        { "/api/flags/ack",     HTTP_POST, h_flags_ack,   NULL },
        { "/api/identify",      HTTP_POST, h_identify,    NULL },
        { "/api/reboot",        HTTP_POST, h_reboot,      NULL },
        { "/api/ota",           HTTP_POST, h_ota,         NULL },
        { "/api/logs",          HTTP_GET,  h_logs,        NULL },
        { "/api/msp/scan",      HTTP_GET,  h_msp_scan,    NULL },
        { "/*",                 HTTP_GET,  h_root,        NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s, &routes[i]);
    }
    ESP_LOGI(TAG, "%d routes registered", (int)(sizeof(routes)/sizeof(routes[0])));
}
