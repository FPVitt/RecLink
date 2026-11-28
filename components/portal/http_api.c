// http_api.c -- REST surface (spec 10.4). All POSTs require !armed (409).
// No auth beyond WPA2 -- documented bench-only limitation (spec 10.4).
#include "portal_internal.h"
#include "portal.h"

#include <string.h>
#include <stdlib.h>

#include "cfg.h"
#include "fc_state.h"
#include "cam_state.h"
#include "cam_caps.h"
#include "camera.h"
#include "ble_central.h"
#include "control.h"
#include "msp.h"
#include "platform.h"
#include "applog.h"
#include "monoclock.h"
#include "rid.h"
#include "osd.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "api";

// One scratch area for the handlers that need a big temporary buffer: the OTA accumulation
// buffer, the log dump and the capability snapshot. httpd runs its handlers one at a time on
// a single task, so they never overlap -- and sharing saves ~4.6 KB of a ~35 KB heap.
#define OTA_BUF 16384
static union {
    uint8_t     ota[OTA_BUF];
    char        logs[APPLOG_RING_BYTES + 1];
    struct { cam_caps_t c; cam_preset_t p[CAM_PRESETS_MAX]; } caps;
    rid_snapshot_t rid;          // GET /api/rid: the Remote ID table copy (spec 8.8.4 / 12.1)
} s_scratch;
_Static_assert(sizeof(((typeof(s_scratch) *)0)->logs) <= OTA_BUF, "log dump must fit the shared scratch");

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
    // The arm bit is forgotten when the FC link drops; the flight hint is not (it also survives a reboot).
    return s.armed || flight_may_be_airborne();
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

    // Streamed section by section (see portal_internal.h): after each FL() the
    // pointers into the sections already sent (m, c, rd, bd, cs, ...) are gone.
    portal_note_client_activity();
    jstream_t js;
    js_open(&js, r);
    cJSON *j = cJSON_CreateObject();
    #define FL() js_flush(&js, j)
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(j, "fw", ad->version);
    cJSON_AddNumberToObject(j, "uptime_s", now_ms() / 1000);
#ifdef CONFIG_BRIDGE_BOARD_RECLINK
    cJSON_AddBoolToObject(j, "reclink_board", true);     // the UI hides the "Open the portal" option
#endif

    cJSON *flags = cJSON_AddArrayToObject(j, "flags");
    uint32_t fl = bridge_flags_all();
    const char *names[] = { "CFG_RESET","REPAIR_NEEDED","OSD_UNSUPPORTED","FC_OFFLINE","CLOCK_SYNC_FAIL","WDT_RECOVERED",
                            "RID_FAULT" };
    for (int i = 0; i < 7; i++) { if (fl & (1u << i)) { cJSON_AddItemToArray(flags, cJSON_CreateString(names[i])); } }

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

    FL();
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

    FL();
    cJSON *c = cJSON_AddObjectToObject(j, "camera");
    cJSON_AddStringToObject(c, "brand", cfg->camera.brand);
    const char *ls_names[] = { "IDLE","SCAN","CONNECTING","BONDING","SYNCING","READY","LOST" };
    cJSON_AddStringToObject(c, "link", ls_names[cam.link <= CAM_LINK_LOST ? cam.link : 0]);
    cJSON_AddBoolToObject(c, "asleep", cam.asleep);
    {   // the user's name for the camera if it has one
        char nick[CAM_NICK_LEN];
        cJSON_AddStringToObject(c, "name", cfg_cam_nick_get(cam.addr, nick) ? nick : cam.name);
    }
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
        char nick[CAM_NICK_LEN];
        cJSON_AddStringToObject(ce, "name", cfg_cam_nick_get(b.addr, nick) ? nick : b.name);   // display name
        cJSON_AddStringToObject(ce, "model", b.name);                                          // the camera's own name
        char bad[20]; snprintf(bad, sizeof(bad), "%02X:%02X:%02X:%02X:%02X:%02X",
                               b.addr[0],b.addr[1],b.addr[2],b.addr[3],b.addr[4],b.addr[5]);
        cJSON_AddStringToObject(ce, "addr", bad);
        bool is_active = memcmp(b.addr, cfg->camera.addr, 6) == 0 && cfg->camera.bonded;
        cJSON_AddBoolToObject(ce, "active", is_active);
        cJSON_AddBoolToObject(ce, "connected", is_active && link_up_now);
        if (cam_is_asleep(b.addr)) { cJSON_AddBoolToObject(ce, "asleep", true); }   // switched off (GoPro advert)
        cJSON_AddNumberToObject(ce, "poll_ds", poll_ds);   // deciseconds
        cJSON_AddBoolToObject(ce, "wake", cfg_cam_wake_on_boot(b.addr));   // wake at power-up (one camera at most)
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
    FL();
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
    FL();
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

    FL();
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
    #define VS(name, field) do { cJSON *o = cJSON_AddObjectToObject(cs, name); \
        cJSON_AddStringToObject(o, "v", cam.field.v); cJSON_AddBoolToObject(o, "valid", cam.field.valid); } while (0)
    VS("iso_max", iso_max); VS("iso_min", iso_min); VS("shutter", shutter);
    VS("protune", protune); VS("wb", wb); VS("color", color); VS("preset_name", preset_name);
    VS("sharpness", sharpness); VS("ev", ev); VS("tune_10bit", tune_10bit);
    #undef VS
    // Raw option values of the settings a zone can push ({"<id>": value}), for the
    // preset builder's "capture from camera"; and how the last zone apply went.
    { cJSON *gs = cJSON_AddObjectToObject(cs, "gp_set");
      for (int i = 0; i < GP_PSET_MAX; i++) {
          if (!cam.gp_set[i].valid) { continue; }
          char key[8]; snprintf(key, sizeof(key), "%u", cam.gp_set[i].id);
          cJSON_AddNumberToObject(gs, key, cam.gp_set[i].v);
      } }
    { static const char *const k_ap[] = { "none", "waiting", "applying", "ok", "partial", "failed" };
      cJSON *ap = cJSON_AddObjectToObject(cs, "zone_apply");
      cJSON_AddNumberToObject(ap, "zone", cam.gp_apply_zone);
      cJSON_AddStringToObject(ap, "state", k_ap[cam.gp_apply_state <= GPA_FAILED ? cam.gp_apply_state : 0]);
      cJSON *fl = cJSON_AddArrayToObject(ap, "fail");
      for (int i = 0; i < GP_PSET_MAX; i++) {
          if (cam.gp_apply_fail & (1u << i)) { cJSON_AddItemToArray(fl, cJSON_CreateNumber(gp_pset_id(i))); }
      } }
    cJSON_AddStringToObject(cs, "model", cam.model);
    if (cam.gp_preset.valid) { cJSON_AddNumberToObject(cs, "preset_id", cam.gp_preset.v); }   // for "capture from camera" in simple zones
    cJSON_AddNumberToObject(cs, "caps_gen", cam_caps_gen());   // portal refetches /api/camera/caps when it changes
    cJSON_AddBoolToObject(cs, "sd_missing", cam.sd_missing);
    cJSON_AddBoolToObject(cs, "sd_full", cam.sd_full);
    cJSON_AddBoolToObject(cs, "hot", cam.hot);
    #undef VU

    // Remote ID receiver (spec 8.8.10 / 10.5). Counters and the summary only -- no IDs, no
    // coordinates (those are in GET /api/rid).
    FL();
    {
        cJSON *rd = cJSON_AddObjectToObject(j, "rid");
        uint32_t now = now_ms();
        bool active = rid_active();
        cJSON_AddBoolToObject(rd, "enabled", cfg->rid.enabled);
        cJSON_AddBoolToObject(rd, "available", rid_available());      // RAM allocated at boot
        // No layout line contains %rp: Remote ID draws nothing on the OSD (the portal warns).
        cJSON_AddBoolToObject(rd, "no_osd_line", cfg->rid.enabled && osd_rp_line(cfg) < 0);
        cJSON_AddBoolToObject(rd, "reboot_required", cfg->rid.enabled && !rid_available() && !rid_alloc_failed());
        cJSON_AddBoolToObject(rd, "active", active);
        char why[40] = "";
        if (!cfg->rid.enabled) { strlcpy(why, "disabled", sizeof(why)); }
        else if (rid_alloc_failed()) { strlcpy(why, "memory allocation failed", sizeof(why)); }
        else if (!rid_available()) { strlcpy(why, "reboot required", sizeof(why)); }
        else if (!active) {
            if (!fc.link_up)                         { strlcpy(why, "no FC link", sizeof(why)); }
            else if (!fc.gps_present)                { strlcpy(why, "no GPS sensor", sizeof(why)); }
            else if (fc.gps.fix < 3)                 { strlcpy(why, "no fix", sizeof(why)); }
            else if (fc.gps.sats < cfg->rid.min_sats) {
                snprintf(why, sizeof(why), "%u of %u satellites", fc.gps.sats, cfg->rid.min_sats);
            } else if (!fc.gps_rx_ms || now - fc.gps_rx_ms > RID_GPS_MAX_AGE_MS) {
                strlcpy(why, "GPS data stale", sizeof(why));
            } else                                   { strlcpy(why, "settling", sizeof(why)); }
        }
        cJSON_AddStringToObject(rd, "inactive_reason", why);
        static const char *const k_mode[] = { "ble", "wifi", "both" };
        cJSON_AddStringToObject(rd, "mode", k_mode[cfg->rid.mode <= 2 ? cfg->rid.mode : 0]);
        cJSON_AddNumberToObject(rd, "min_sats", cfg->rid.min_sats);
        bool hv = fc.att_valid && fc.hdg_aligned;
        cJSON_AddStringToObject(rd, "hdg", !hv ? "none" : fc.mag_present ? "mag" : "gps");
        cJSON_AddBoolToObject(rd, "hdg_aligned", fc.hdg_aligned);
        cJSON_AddBoolToObject(rd, "mag_present", fc.mag_present);
        rid_counters_t rc; rid_counters_get(&rc);
        cJSON *rb = cJSON_AddObjectToObject(rd, "ble");
        cJSON_AddStringToObject(rb, "scan", rid_scan_running() ? "on" : "off");
        cJSON_AddNumberToObject(rb, "rx", rc.ble_rx);
        cJSON_AddNumberToObject(rb, "drops", rc.ble_drops);
        cJSON *rw = cJSON_AddObjectToObject(rd, "wifi");
        static const char *const k_ws[] = { "off", "ap", "rx", "test" };
        uint8_t ws = rid_wifi_state();
        cJSON_AddStringToObject(rw, "state", k_ws[ws <= RID_WIFI_TEST ? ws : 0]);
        cJSON_AddNumberToObject(rw, "ch", 6);
        cJSON_AddNumberToObject(rw, "rx", rc.wifi_rx);
        cJSON_AddNumberToObject(rw, "drops", rc.wifi_drops);
        cJSON_AddNumberToObject(rw, "frames_30s", rid_wifi_frames_30s());
        cJSON_AddNumberToObject(rw, "beacon", rc.beacon);
        cJSON_AddNumberToObject(rw, "nan", rc.nan);
        rid_summary_t sm; rid_summary_get(&sm);
        static const char *const k_lvl[] = { "none", "notice", "warn", "alert" };
        cJSON_AddNumberToObject(rd, "count", active ? sm.count : 0);
        if (active && (sm.flags & RID_SUM_HAVE)) {
            cJSON *nr = cJSON_AddObjectToObject(rd, "nearest");
            cJSON_AddNumberToObject(nr, "dist_m", sm.n_msg ? sm.msg[0].dist_m : sm.near_dist_m);
            cJSON_AddStringToObject(nr, "level", k_lvl[sm.level <= 3 ? sm.level : 0]);
        }
        cJSON_AddNumberToObject(rd, "decoded", rc.decoded);
        cJSON_AddNumberToObject(rd, "jump_rejects", rc.jump_rejects);
        cJSON_AddNumberToObject(rd, "range_rejects", rc.range_rejects);
        cJSON_AddNumberToObject(rd, "evict_drops", rc.evict_drops);
        uint8_t fm = rid_fault_mask(cfg, now);
        cJSON *rf = cJSON_AddObjectToObject(rd, "fault");
        cJSON_AddBoolToObject(rf, "subsystem", (fm & RID_FAULT_SUBSYS) != 0 || bridge_flag_get(FLAG_RID_FAULT));
        cJSON_AddBoolToObject(rf, "ble", (fm & RID_FAULT_BLE) != 0);
        cJSON_AddBoolToObject(rf, "wifi", (fm & RID_FAULT_WIFI) != 0);
        rid_test_status_t ts; rid_test_status(&ts, now);
        cJSON *rt = cJSON_AddObjectToObject(rd, "test");
        static const char *const k_ts[] = { "idle", "run", "done" };
        cJSON_AddStringToObject(rt, "state", k_ts[ts.state <= 2 ? ts.state : 0]);
        cJSON_AddBoolToObject(rt, "wifi", ts.wifi);
        cJSON_AddBoolToObject(rt, "aborted", ts.aborted);
        cJSON_AddNumberToObject(rt, "secs_left", ts.secs_left);
        cJSON_AddNumberToObject(rt, "ble_frames", ts.ble_frames);
        cJSON_AddNumberToObject(rt, "wifi_frames", ts.wifi_frames);
        cJSON_AddNumberToObject(rt, "beacon", ts.beacon);
        cJSON_AddNumberToObject(rt, "nan", ts.nan);
        cJSON_AddNumberToObject(rt, "aircraft", ts.aircraft);
    }

    FL();
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

    #undef FL
    return js_close(&js, j);
}

// ---- GET/POST /api/config --------------------------------------
// Optional ?cam=<12 hex digits of a bonded camera's address, colons allowed>: the Cam control, OSD,
// Remote ID and Clock settings are per camera (cfg.h). Without it the request is about the active
// camera, as before. Returns 1 = a valid cam was given (a[] filled), 0 = none, -1 = malformed.
static int cam_query(httpd_req_t *r, uint8_t a[6])
{
    char q[64], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "cam", v, sizeof(v)) != ESP_OK) { return 0; }
    int n = 0;
    for (const char *p = v; *p; p++) {
        if (*p == ':') { continue; }
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
              : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        if (d < 0 || n >= 12) { return -1; }
        if (n & 1) { a[n >> 1] |= (uint8_t)d; } else { a[n >> 1] = (uint8_t)(d << 4); }
        n++;
    }
    return n == 12 ? 1 : -1;
}

static esp_err_t h_config_get(httpd_req_t *r)
{
    portal_note_client_activity();
    uint8_t a[6];
    int cq = cam_query(r, a);
    if (cq < 0) { httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"bad cam\"}"); return ESP_OK; }
    if (cq > 0) {
        cfg_t tmp; bool stored = false;
        if (cfg_cam_settings_get(a, &tmp, &stored) != ESP_OK) {
            httpd_resp_set_status(r, "404 Not Found"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"no such camera\"}"); return ESP_OK;
        }
        httpd_resp_set_hdr(r, "X-Cam-Stored", stored ? "1" : "0");   // 0: showing the active camera's settings until its first save
        return cfg_to_json_stream(&tmp, r);
    }
    return cfg_to_json_stream(cfg_get(), r);
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

    uint8_t cam[6];
    int cq = cam_query(r, cam);
    cfg_t cand;
    if (cq > 0) {
        bool st;
        if (cfg_cam_settings_get(cam, &cand, &st) != ESP_OK) {       // the camera's own sections + the live globals
            cJSON_Delete(j);
            httpd_resp_set_status(r, "404 Not Found"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"no such camera\"}");
            return ESP_OK;
        }
    } else if (cq < 0) {
        cJSON_Delete(j);
        httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"bad cam\"}");
        return ESP_OK;
    } else {
        cfg_snapshot(&cand);
    }
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
    e = cq > 0 ? cfg_cam_settings_commit(cam, &cand)      // that camera's sections + the globals
               : cfg_commit(&cand);                       // synchronous NVS commit (spec 11.0)

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    cJSON_AddBoolToObject(resp, "applied", e == ESP_OK);
    // Remote ID RAM is allocated only at boot: rid.enabled 0 -> 1 is persisted but takes effect at
    // the next reboot (1 -> 0 applies at once).
    if (e == ESP_OK && cand.rid.enabled && !rid_available()) {
        cJSON_AddBoolToObject(resp, "reboot_required", true);
    }
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
// {"addr":"AA:BB:..","name":"..."} -> give a paired camera a name ("" clears it).
static esp_err_t h_cam_name(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 160);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ad = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    const cJSON *nm = j ? cJSON_GetObjectItem(j, "name") : NULL;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    uint8_t a[6];
    if (cJSON_IsString(ad) && cJSON_IsString(nm) &&
        sscanf(ad->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        e = cfg_cam_nick_set(a, nm->valuestring);
    }
    cJSON_Delete(j);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    return send_json(r, resp, e == ESP_OK ? NULL : e == ESP_ERR_NOT_FOUND ? "404 Not Found" : "400 Bad Request");
}

static esp_err_t h_cam_poll(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ad = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    const cJSON *ps = j ? cJSON_GetObjectItem(j, "poll_ds") : NULL;
    const cJSON *wk = j ? cJSON_GetObjectItem(j, "wake") : NULL;      // wake at power-up (this camera only)
    esp_err_t e = ESP_ERR_INVALID_ARG;
    uint8_t a[6];
    if (cJSON_IsString(ad) && (cJSON_IsNumber(ps) || cJSON_IsBool(wk)) &&
        sscanf(ad->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        e = ESP_OK;
        if (cJSON_IsNumber(ps)) {
            int v = (int)ps->valuedouble;
            if (v < CAM_POLL_DS_MIN) { v = CAM_POLL_DS_MIN; }
            if (v > CAM_POLL_DS_MAX) { v = CAM_POLL_DS_MAX; }
            e = cfg_cam_set_poll(a, (uint8_t)v);
        }
        if (e == ESP_OK && cJSON_IsBool(wk)) { e = cfg_cam_set_wake(a, cJSON_IsTrue(wk)); }
    }
    cJSON_Delete(j);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    return send_json(r, resp, e == ESP_OK ? NULL : "400 Bad Request");
}

// GET /api/camera/caps -- what each preset-able setting may currently be set to,
// as the camera reported it: {"gen":N,"caps":{"<setting id>":[option values],...}}.
// Settings the camera did not list are absent. Streamed by hand: as a cJSON tree
// this is ~400 nodes, far too much for the heap.
static esp_err_t h_cam_caps(httpd_req_t *r)
{
    cam_caps_t *caps = &s_scratch.caps.c;   // shared scratch: httpd runs one handler at a time
    cam_caps_get(caps);
    int np = cam_caps_presets(s_scratch.caps.p, CAM_PRESETS_MAX);
    portal_note_client_activity();
    jstream_t js;
    js_open(&js, r);
    char b[160];
    int n = snprintf(b, sizeof(b), "\"gen\":%u,\"caps\":{", (unsigned)caps->gen);
    js_put(&js, b, n);
    bool first = true;
    for (int i = 0; i < GP_PSET_MAX; i++) {
        if (!caps->have[i]) { continue; }
        n = snprintf(b, sizeof(b), "%s\"%u\":[", first ? "" : ",", gp_pset_id(i));
        first = false;
        bool comma = false;
        for (int v = 0; v < 256; v++) {
            if (!cam_caps_has(caps, i, (uint8_t)v)) { continue; }
            if (n > (int)sizeof(b) - 6) { js_put(&js, b, n); n = 0; }
            n += snprintf(b + n, sizeof(b) - n, "%s%d", comma ? "," : "", v);
            comma = true;
        }
        n += snprintf(b + n, sizeof(b) - n, "]");
        js_put(&js, b, n);
    }
    js_put(&js, "}", 1);
    // The camera's video presets, [[id,"name"],...] (names are sanitised printable ASCII).
    js_put(&js, ",\"presets\":[", 12);
    for (int i = 0; i < np; i++) {
        n = snprintf(b, sizeof(b), "%s[%lu,\"%s\"]", i ? "," : "",
                     (unsigned long)s_scratch.caps.p[i].id, s_scratch.caps.p[i].name);
        js_put(&js, b, n);
    }
    js_put(&js, "]", 1);
    return js_close(&js, cJSON_CreateObject());
}

// POST /api/camera/set {"id":N,"val":V} -- write ONE preset-able setting to the
// camera now (the builder uses it to keep the camera's options in step with the
// zone being edited). Explicit portal action: refused while armed; the driver
// refuses while recording.
static esp_err_t h_cam_set(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    char *body = read_body(r, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ji = j ? cJSON_GetObjectItem(j, "id") : NULL;
    const cJSON *jv = j ? cJSON_GetObjectItem(j, "val") : NULL;
    int id = cJSON_IsNumber(ji) ? (int)ji->valuedouble : -1;
    int val = cJSON_IsNumber(jv) ? (int)jv->valuedouble : -1;
    cJSON_Delete(j);

    cJSON *resp = cJSON_CreateObject();
    if (id < 1 || id > 255 || val < 0 || val > 255 || gp_pset_index((uint8_t)id) < 0) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "not a settable id/value");
        return send_json(r, resp, "400 Bad Request");
    }
    cam_submit(CAM_OP_SET_SETTING, (id << 8) | val);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(r, resp, NULL);
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
    if (slot < 0 || slot >= cfg_zones(cfg_get())) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "no such zone");
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
    cam_state_t cam; cam_state_get(&cam);
    if (cam.link != CAM_LINK_READY && cam.asleep) {
        // Asleep GoPro: wake it; the page runs the test again once the camera is connected.
        cam_wake_now();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddBoolToObject(resp, "waking", true);
        return send_json(r, resp, NULL);
    }
    cam_submit(CAM_OP_RECORD_START, 0);
    // One timer, reused (a new one per press was never deleted).
    static esp_timer_handle_t s_test_timer;
    if (!s_test_timer) {
        const esp_timer_create_args_t ta = { .callback = test_stop_cb, .name = "camtest" };
        if (esp_timer_create(&ta, &s_test_timer) != ESP_OK) { s_test_timer = NULL; }
    }
    if (s_test_timer) {
        esp_timer_stop(s_test_timer);                    // restart the 3 s if pressed again
        esp_timer_start_once(s_test_timer, 3000000);
    }
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

// ---- Remote ID (spec 8.8.10) ------------------------------------
// GET /api/rid -- the aircraft table, streamed by hand (no whole-response cJSON tree). The table
// is copied with a 0-timeout take into the shared scratch: a blocked handler would hang
// httpd_stop() at arming and keep RID_RX from ever starting -> 503 instead. Per aircraft ONLY
// {id, src, age_ms, ignored} + {level, dist_m, brg} when confirmed (rid_ac_json; data minimisation).
static esp_err_t h_rid(httpd_req_t *r)
{
    portal_note_client_activity();
    rid_snapshot_t *sn = &s_scratch.rid;
    if (rid_available() && !rid_table_try_copy(sn)) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Retry-After", "1");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"busy\"}");
    }
    if (!rid_available()) { memset(sn, 0, sizeof(*sn)); }   // never allocated: empty, inactive
    fc_state_t fc; fc_state_get(&fc);
    bool active = rid_active();
    bool hv = fc.att_valid && fc.hdg_aligned;
    uint32_t now = now_ms();

    jstream_t js;
    js_open(&js, r);
    char b[RID_AC_JSON_MAX];
    int n = snprintf(b, sizeof(b), "\"active\":%s,\"hdg\":\"%s\",\"ac\":[",
                     active ? "true" : "false", !hv ? "none" : fc.mag_present ? "mag" : "gps");
    js_put(&js, b, (size_t)n);
    bool first = true;
    for (int i = 0; active && i < RID_MAX_AC; i++) {
        if (!sn->ac[i].used) { continue; }
        if (!first) { js_put(&js, ",", 1); }
        first = false;
        n = rid_ac_json(b, sizeof(b), &sn->ac[i], now, true, (uint8_t)(i + 1));
        js_put(&js, b, (size_t)n);
    }
    js_put(&js, "]", 1);
    return js_close(&js, cJSON_CreateObject());
}

// GET /api/rid/test -- the reception test's counters and the aircraft heard during it (live while
// it runs, frozen after it ends until the next test, a reboot, or rid.enabled = 0). Per aircraft
// ONLY {id, src, age_ms} + {dist_m, brg} with a confirmed own position. Streamed by hand; 0-timeout
// take (503 + Retry-After when busy).
static esp_err_t h_rid_test_get(httpd_req_t *r)
{
    portal_note_client_activity();
    uint32_t now = now_ms();
    rid_snapshot_t *sn = &s_scratch.rid;
    if (rid_available() && !rid_test_try_copy(sn)) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Retry-After", "1");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"busy\"}");
    }
    if (!rid_available()) { memset(sn, 0, sizeof(*sn)); }
    rid_test_status_t ts; rid_test_status(&ts, now);
    static const char *const k_ts[] = { "idle", "run", "done" };

    jstream_t js;
    js_open(&js, r);
    char b[RID_AC_JSON_MAX];
    int n = snprintf(b, sizeof(b),
        "\"state\":\"%s\",\"secs_left\":%u,\"wifi\":%s,\"aborted\":%s,\"ble_frames\":%lu,"
        "\"wifi_frames\":%lu,\"beacon\":%lu,\"nan\":%lu,\"aircraft\":%u,\"ac\":[",
        k_ts[ts.state <= 2 ? ts.state : 0], ts.secs_left, ts.wifi ? "true" : "false",
        ts.aborted ? "true" : "false", (unsigned long)ts.ble_frames, (unsigned long)ts.wifi_frames,
        (unsigned long)ts.beacon, (unsigned long)ts.nan, ts.aircraft);
    js_put(&js, b, (size_t)n);
    bool first = true;
    for (int i = 0; ts.state != RID_TEST_IDLE && i < RID_MAX_AC; i++) {
        if (!sn->ac[i].used) { continue; }
        if (!first) { js_put(&js, ",", 1); }
        first = false;
        n = rid_ac_json(b, sizeof(b), &sn->ac[i], now, false, (uint8_t)(i + 1));
        js_put(&js, b, (size_t)n);
    }
    js_put(&js, "]", 1);
    return js_close(&js, cJSON_CreateObject());
}

// POST /api/rid/test {"secs":30} -- reception test (spec 8.8.10): BLE always, plus Wi-Fi (which
// drops the AP: the phone disconnects) when rid.mode includes it. Needs no GPS fix; counters only;
// aborts if the aircraft arms. 409 armed, 412 rid.enabled off.
static esp_err_t h_rid_test(httpd_req_t *r)
{
    if (reject_if_armed(r) != ESP_OK) { return ESP_OK; }
    const cfg_t *c = cfg_get();
    if (!c->rid.enabled) {
        httpd_resp_set_status(r, "412 Precondition Failed");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"remote id disabled\"}");
    }
    if (!rid_available()) {        // RAM is only allocated at boot (enabled then): reboot first
        httpd_resp_set_status(r, "412 Precondition Failed");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, rid_alloc_failed()
            ? "{\"ok\":false,\"error\":\"remote id memory allocation failed at boot\"}"
            : "{\"ok\":false,\"error\":\"remote id was not enabled at boot -- reboot first\",\"reboot_required\":true}");
    }
    int secs = 30;
    char *body = read_body(r, 64);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *js = j ? cJSON_GetObjectItem(j, "secs") : NULL;
    if (cJSON_IsNumber(js)) { secs = (int)js->valuedouble; }
    cJSON_Delete(j);
    if (secs < 5) { secs = 5; }
    if (secs > 60) { secs = 60; }
    bool wifi = rid_mode_has_wifi(c->rid.mode);
    bool started = rid_test_start((uint16_t)secs, wifi, now_ms());
    portal_wifi_kick();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "started", started);       // false: one is already running
    cJSON_AddNumberToObject(resp, "secs", secs);
    cJSON_AddBoolToObject(resp, "wifi", wifi);              // true: the AP goes down now
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
    char *buf = s_scratch.logs;
    size_t n = applog_dump(buf, sizeof(s_scratch.logs));
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
    uint8_t *buf = s_scratch.ota;

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
        { "/api/camera/name",   HTTP_POST, h_cam_name,    NULL },
        { "/api/camera/preset", HTTP_POST, h_cam_preset,  NULL },
        { "/api/camera/caps",   HTTP_GET,  h_cam_caps,    NULL },
        { "/api/camera/set",    HTTP_POST, h_cam_set,     NULL },
        { "/api/camera/test",   HTTP_POST, h_cam_test,    NULL },
        { "/api/clock/sync",    HTTP_POST, h_clock_sync,  NULL },
        { "/api/flags/ack",     HTTP_POST, h_flags_ack,   NULL },
        { "/api/identify",      HTTP_POST, h_identify,    NULL },
        { "/api/reboot",        HTTP_POST, h_reboot,      NULL },
        { "/api/ota",           HTTP_POST, h_ota,         NULL },
        { "/api/logs",          HTTP_GET,  h_logs,        NULL },
        { "/api/msp/scan",      HTTP_GET,  h_msp_scan,    NULL },
        { "/api/rid",           HTTP_GET,  h_rid,         NULL },
        { "/api/rid/test",      HTTP_POST, h_rid_test,    NULL },
        { "/api/rid/test",      HTTP_GET,  h_rid_test_get, NULL },
        { "/*",                 HTTP_GET,  h_root,        NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s, &routes[i]);
    }
    ESP_LOGI(TAG, "%d routes registered", (int)(sizeof(routes)/sizeof(routes[0])));
}
