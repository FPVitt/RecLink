// Per-camera settings (cfg.h "per-camera settings"): the Cam control / OSD / Remote ID / Clock
// sections follow the ACTIVE camera. The head fields of cfg_t always hold the active camera's
// settings; a camera switch saves the outgoing camera's settings under its own NVS key and loads the
// incoming camera's. Through the real cfg.c with an in-RAM keyed NVS.
#include "test.h"
#include "cfg.h"
#include "crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stddef.h>

// ---- in-RAM keyed NVS ---------------------------------------------------------------------
#define MAXK 16
static struct { char key[16]; uint8_t d[1024]; size_t len; bool used; } g_kv[MAXK];
static int g_writes;
static int kv_find(const char *k) { for (int i = 0; i < MAXK; i++) { if (g_kv[i].used && !strcmp(g_kv[i].key, k)) { return i; } } return -1; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(g_kv, 0, sizeof g_kv); return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h; int i = kv_find(k);
    if (i < 0) { for (i = 0; i < MAXK && g_kv[i].used; i++) { } if (i >= MAXK) { return ESP_ERR_NO_MEM; } }
    strcpy(g_kv[i].key, k); memcpy(g_kv[i].d, v, len); g_kv[i].len = len; g_kv[i].used = true; g_writes++;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h; int i = kv_find(k);
    if (i < 0) { return ESP_ERR_NOT_FOUND; }
    size_t n = g_kv[i].len < *len ? g_kv[i].len : *len;
    memcpy(out, g_kv[i].d, n); *len = g_kv[i].len;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; memset(g_kv, 0, sizeof g_kv); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; int i = kv_find(k); if (i >= 0) { g_kv[i].used = false; } return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static const uint8_t A[6] = { 0xD4, 0x5E, 0x1A, 0x7C, 0x02, 0x9B };
static const uint8_t B[6] = { 0xF0, 0x8C, 0x3E, 0x41, 0xA7, 0x60 };
static const uint8_t C3[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static void fresh(void)
{
    memset(g_kv, 0, sizeof g_kv);
    bool r, o;
    CHECK_EQ(cfg_init(&r, &o), ESP_OK);
}

static bool active_is(const uint8_t a[6]) { return cfg_get()->camera.bonded && !memcmp(cfg_get()->camera.addr, a, 6); }

static void edit_active(const char *line0, uint8_t rec_mode, uint8_t min_sats, uint8_t rid_on)
{
    cfg_t c; cfg_snapshot(&c);
    strlcpy(c.osd.line[0], line0, sizeof c.osd.line[0]);
    c.record.mode = rec_mode; c.clock.min_sats = min_sats; c.rid.enabled = rid_on;
    CHECK_EQ(cfg_commit(&c), ESP_OK);
}

static void test_profile_pure(void)
{
    cfg_t c; cfg_load_defaults(&c);
    c.record.mode = REC_SWITCH; c.osd.refresh_hz = 7; c.clock.min_sats = 9; c.n_zones = 5;
    c.gp_preset_x[1].n = 2; c.bands_x[2].max_us = 1999; c.hilight.channel = 6;
    cfg_rid_seal(&c);
    cfg_profile_t p; cfg_profile_extract(&c, &p);
    CHECK(cfg_profile_valid(&p));
    cfg_t d; cfg_load_defaults(&d);
    CHECK(cfg_profile_apply(&d, &p));
    CHECK_EQ(d.record.mode, REC_SWITCH); CHECK_EQ(d.osd.refresh_hz, 7); CHECK_EQ(d.clock.min_sats, 9);
    CHECK_EQ(d.n_zones, 5); CHECK_EQ(d.gp_preset_x[1].n, 2); CHECK_EQ(d.bands_x[2].max_us, 1999); CHECK_EQ(d.hilight.channel, 6);
    CHECK_EQ(d.wifi.portal_timeout_min, c.wifi.portal_timeout_min);   // untouched: not a camera section
    p.osd.refresh_hz ^= 1;                                            // corrupt: CRC no longer matches
    cfg_t e; cfg_load_defaults(&e);
    const uint8_t hz = e.osd.refresh_hz;
    CHECK(!cfg_profile_apply(&e, &p));
    CHECK_EQ(e.osd.refresh_hz, hz);                                   // *e untouched
}

// A "simple" GoPro zone (n == GP_ZONE_PRESET) survives validation and a commit, carries its preset id
// in bands[].id, never exposes settings, and an ordinary zone is still cleaned as before.
static void test_simple_gopro_zone(void)
{
    fresh();
    cfg_t c; cfg_snapshot(&c);
    cfg_gp_zone_t *z0 = cfg_zone_gp_mut(&c, 0), *z1 = cfg_zone_gp_mut(&c, 1), *z2 = cfg_zone_gp_mut(&c, 2);
    memset(z0, 0, sizeof *z0); z0->n = GP_ZONE_PRESET; z0->s[0].id = 2; z0->s[0].val = 9;     // junk behind the marker
    cfg_zone_set(&c, 0, cfg_zone_max_us(&c, 0), 65540u);                                        // a GoPro preset id (> 255)
    z1->n = 2; z1->s[0].id = 3; z1->s[0].val = 1; z1->s[1].id = 250; z1->s[1].val = 1;         // 250 is not whitelisted
    z2->n = 99;                                                                                 // out of range: dropped
    char err[64];
    CHECK_EQ(cfg_validate(&c, err, sizeof err), ESP_OK);
    CHECK(cfg_gp_zone_is_preset(cfg_zone_gp(&c, 0)));
    CHECK_EQ(cfg_gp_zone_n(cfg_zone_gp(&c, 0)), 0);
    CHECK_EQ(cfg_zone_gp(&c, 0)->s[0].id, 0);                    // the junk behind the marker is gone
    CHECK_EQ(cfg_zone_id(&c, 0), 65540u);                        // the preset id is kept as is
    CHECK(!cfg_gp_zone_is_preset(cfg_zone_gp(&c, 1)));
    CHECK_EQ(cfg_gp_zone_n(cfg_zone_gp(&c, 1)), 1);              // only the whitelisted setting stays
    CHECK_EQ(cfg_gp_zone_n(cfg_zone_gp(&c, 2)), 0);
    CHECK(!cfg_gp_zone_is_preset(cfg_zone_gp(&c, 2)));
    CHECK_EQ(cfg_commit(&c), ESP_OK);
    CHECK(cfg_gp_zone_is_preset(cfg_zone_gp(cfg_get(), 0)));     // survives the commit
    CHECK_EQ(cfg_zone_id(cfg_get(), 0), 65540u);
    cfg_gp_zone_t g = { .n = 5 };
    CHECK_EQ(cfg_gp_zone_n(&g), 5);
    g.n = GP_ZONE_PRESET;
    CHECK_EQ(cfg_gp_zone_n(&g), 0);                              // never indexes past s[]
}

// Retired OSD tokens (%bb %cn %gs %gf %fb %sp) are dropped from saved lines, also when a profile is applied.
static void test_retired_tokens(void)
{
    cfg_t c; cfg_load_defaults(&c);
    strlcpy(c.osd.line[0], "%st %sp %et", sizeof c.osd.line[0]);
    strlcpy(c.osd.line[1], "%bb%bt %gs%gf", sizeof c.osd.line[1]);
    strlcpy(c.osd.line[2], "%cn %fb %bi %cm", sizeof c.osd.line[2]);
    strlcpy(c.osd.line[3], "SD %sd %sp%sp", sizeof c.osd.line[3]);
    char err[64]; CHECK_EQ(cfg_validate(&c, err, sizeof err), ESP_OK);
    CHECK(strcmp(c.osd.line[0], "%st  %et") == 0);
    CHECK(strcmp(c.osd.line[1], "%bt ") == 0);
    CHECK(strcmp(c.osd.line[2], "  %bi %cm") == 0);
    CHECK(strcmp(c.osd.line[3], "SD %sd ") == 0);
    cfg_t d; cfg_load_defaults(&d);
    strlcpy(d.osd.line[0], "%bb%st", sizeof d.osd.line[0]);
    cfg_profile_t p; cfg_profile_extract(&d, &p);
    cfg_t e; cfg_load_defaults(&e);
    CHECK(cfg_profile_apply(&e, &p));
    CHECK(strcmp(e.osd.line[0], "%st") == 0);
}

static void test_switch_saves_and_loads(void)
{
    fresh();
    CHECK_EQ(cfg_set_camera_bond("gopro", A, "HERO11", 0), ESP_OK);
    CHECK(active_is(A));
    edit_active("A-LINE", REC_ARM, 6, 0);
    // pair a second camera: it inherits A's settings, A's are saved under its own key
    CHECK_EQ(cfg_set_camera_bond("dji", B, "Osmo", 0x1234), ESP_OK);
    CHECK(active_is(B));
    CHECK(strcmp(cfg_get()->osd.line[0], "A-LINE") == 0);             // inherited
    CHECK(kv_find("pd45e1a7c029b") >= 0);                             // A's profile exists
    edit_active("B-LINE", REC_SWITCH, 8, 0);
    CHECK_EQ(cfg_set_active_camera(A), ESP_OK);                       // A connects
    CHECK(active_is(A));
    CHECK(strcmp(cfg_get()->osd.line[0], "A-LINE") == 0);
    CHECK_EQ(cfg_get()->record.mode, REC_ARM); CHECK_EQ(cfg_get()->clock.min_sats, 6);
    CHECK_EQ(cfg_set_active_camera(B), ESP_OK);                       // B connects
    CHECK(strcmp(cfg_get()->osd.line[0], "B-LINE") == 0);
    CHECK_EQ(cfg_get()->record.mode, REC_SWITCH); CHECK_EQ(cfg_get()->clock.min_sats, 8);
    // global settings do not follow the camera
    cfg_t c; cfg_snapshot(&c); strlcpy(c.wifi.ssid, "MYNET", sizeof c.wifi.ssid); CHECK_EQ(cfg_commit(&c), ESP_OK);
    CHECK_EQ(cfg_set_active_camera(A), ESP_OK);
    CHECK(strcmp(cfg_get()->wifi.ssid, "MYNET") == 0);
    // the bond list and the active camera are never lost
    CHECK_EQ(cfg_cam_count(), 2);
}

static void test_edit_inactive_camera(void)
{
    fresh();
    cfg_set_camera_bond("gopro", A, "HERO11", 0); edit_active("A-LINE", REC_ARM, 6, 0);
    cfg_set_camera_bond("dji", B, "Osmo", 1); edit_active("B-LINE", REC_SWITCH, 8, 0);
    cfg_set_active_camera(A);
    cfg_t c; bool stored = false;
    CHECK_EQ(cfg_cam_settings_get(B, &c, &stored), ESP_OK);
    CHECK(stored); CHECK(strcmp(c.osd.line[0], "B-LINE") == 0); CHECK_EQ(c.record.mode, REC_SWITCH);
    strlcpy(c.osd.line[0], "B-EDIT", sizeof c.osd.line[0]); c.clock.min_sats = 11;
    strlcpy(c.wifi.ssid, "SHARED", sizeof c.wifi.ssid);              // a global field edited in the same save
    CHECK_EQ(cfg_cam_settings_commit(B, &c), ESP_OK);
    CHECK(active_is(A));
    CHECK(strcmp(cfg_get()->osd.line[0], "A-LINE") == 0);             // the active camera is untouched
    CHECK_EQ(cfg_get()->clock.min_sats, 6);
    CHECK(strcmp(cfg_get()->wifi.ssid, "SHARED") == 0);               // the global change did land
    CHECK_EQ(cfg_set_active_camera(B), ESP_OK);
    CHECK(strcmp(cfg_get()->osd.line[0], "B-EDIT") == 0); CHECK_EQ(cfg_get()->clock.min_sats, 11);
    // the active camera through the same call is the ordinary commit
    cfg_snapshot(&c); c.record.mode = REC_OFF;
    CHECK_EQ(cfg_cam_settings_commit(B, &c), ESP_OK); CHECK_EQ(cfg_get()->record.mode, REC_OFF);
    // unknown camera
    CHECK_EQ(cfg_cam_settings_commit(C3, &c), ESP_ERR_NOT_FOUND);
    CHECK_EQ(cfg_cam_settings_get(C3, &c, &stored), ESP_ERR_NOT_FOUND);
}

static void test_no_profile_inherits(void)
{
    fresh();
    cfg_set_camera_bond("gopro", A, "HERO11", 0); edit_active("A-LINE", REC_ARM, 6, 0);
    cfg_set_camera_bond("dji", B, "Osmo", 1);                        // B active, A saved
    int ia = kv_find("pd45e1a7c029b"); CHECK(ia >= 0); g_kv[ia].used = false;   // an old bond with no stored profile
    cfg_t c; bool stored = true;
    CHECK_EQ(cfg_cam_settings_get(A, &c, &stored), ESP_OK);
    CHECK(!stored); CHECK(strcmp(c.osd.line[0], "A-LINE") == 0);      // shows the live (B's, inherited) settings
    edit_active("B-ONLY", REC_SWITCH, 9, 0);
    CHECK_EQ(cfg_set_active_camera(A), ESP_OK);                       // A has no profile: keeps what is in the head
    CHECK(strcmp(cfg_get()->osd.line[0], "B-ONLY") == 0);
    // a corrupt profile counts as absent
    cfg_set_active_camera(B);                                         // saves A (= B-ONLY copy) ...
    int ib = kv_find("pd45e1a7c029b"); CHECK(ib >= 0);
    g_kv[ib].d[20] ^= 0xFF;                                           // ... then corrupt it
    edit_active("B-AGAIN", REC_ARM, 7, 0);
    CHECK_EQ(cfg_set_active_camera(A), ESP_OK);
    CHECK(strcmp(cfg_get()->osd.line[0], "B-AGAIN") == 0);            // inherited, not garbage
}

static void test_forget_and_clear(void)
{
    fresh();
    cfg_set_camera_bond("gopro", A, "HERO11", 0); edit_active("A-LINE", REC_ARM, 6, 0);
    cfg_set_camera_bond("dji", B, "Osmo", 1); edit_active("B-LINE", REC_SWITCH, 8, 0);
    cfg_set_camera_bond("gopro", C3, "HERO9", 0); edit_active("C-LINE", REC_OFF, 5, 0);
    CHECK(kv_find("pd45e1a7c029b") >= 0 && kv_find("pf08c3e41a760") >= 0);
    // forgetting a camera that is not active drops only its settings
    CHECK_EQ(cfg_forget_camera(B), ESP_OK);
    CHECK(kv_find("pf08c3e41a760") < 0); CHECK(kv_find("pd45e1a7c029b") >= 0);
    CHECK(strcmp(cfg_get()->osd.line[0], "C-LINE") == 0);
    // forgetting the ACTIVE camera: the new active camera's own settings are loaded
    CHECK_EQ(cfg_forget_camera(C3), ESP_OK);
    CHECK(active_is(A));
    CHECK(strcmp(cfg_get()->osd.line[0], "A-LINE") == 0); CHECK_EQ(cfg_get()->record.mode, REC_ARM);
    CHECK_EQ(cfg_set_camera_bond("dji", B, "Osmo", 1), ESP_OK);       // re-pairing B starts from A's copy
    CHECK(strcmp(cfg_get()->osd.line[0], "A-LINE") == 0);
    CHECK_EQ(cfg_clear_camera(), ESP_OK);
    CHECK(kv_find("pd45e1a7c029b") < 0); CHECK_EQ(cfg_cam_count(), 0);
}

static void test_rid_any_enabled(void)
{
    fresh();
    cfg_set_camera_bond("gopro", A, "HERO11", 0);
    CHECK(!cfg_rid_any_enabled());
    cfg_set_camera_bond("dji", B, "Osmo", 1);
    edit_active("B", REC_ARM, 6, 1);                                  // Remote ID on for B only
    CHECK(cfg_rid_any_enabled());
    cfg_set_active_camera(A);                                         // A active (rid off), B stored on
    CHECK_EQ(cfg_get()->rid.enabled, 0);
    CHECK(cfg_rid_any_enabled());                                     // the boot allocation must still happen
    cfg_t c; bool st; cfg_cam_settings_get(B, &c, &st); c.rid.enabled = 0;
    CHECK_EQ(cfg_cam_settings_commit(B, &c), ESP_OK);
    CHECK(!cfg_rid_any_enabled());
    // the Remote ID tail's own CRC protects a stored profile too: a flipped copy loads DISABLED
    cfg_t d; cfg_snapshot(&d); d.rid.enabled = 1; d.rid.warn.alert_m = 150; cfg_rid_seal(&d);
    cfg_profile_t p; cfg_profile_extract(&d, &p);
    p.rid.mode = RID_MODE_BOTH;                                       // flipped after sealing...
    p.crc32 = crc32_iso_hdlc((const uint8_t *)&p, offsetof(cfg_profile_t, crc32));   // ...profile CRC repaired
    nvs_handle_t h; nvs_open("cfg", NVS_READWRITE, &h); nvs_set_blob(h, "pf08c3e41a760", &p, sizeof p);
    cfg_t out; cfg_snapshot(&out); cfg_profile_apply(&out, &p);
    CHECK_EQ(out.rid.enabled, 0);                                     // tail CRC mismatch -> defaults
    CHECK(!cfg_rid_any_enabled());
}

static void test_nicknames(void)
{
    fresh();
    char n[CAM_NICK_LEN];
    CHECK(!cfg_cam_nick_get(A, n));
    CHECK_EQ(cfg_cam_nick_set(A, "x"), ESP_ERR_NOT_FOUND);              // not bonded
    cfg_set_camera_bond("gopro", A, "HERO11", 0);
    cfg_set_camera_bond("dji", B, "Osmo", 1);
    CHECK_EQ(cfg_cam_nick_set(A, "   Cinewhoop \x01\x7f "), ESP_OK);    // trimmed, control chars dropped
    CHECK(cfg_cam_nick_get(A, n)); CHECK(strcmp(n, "Cinewhoop") == 0);
    CHECK(!cfg_cam_nick_get(B, n));
    CHECK_EQ(cfg_cam_nick_set(B, "0123456789012345678901234567890"), ESP_OK);   // too long: cut to 23
    CHECK(cfg_cam_nick_get(B, n)); CHECK_EQ(strlen(n), (size_t)(CAM_NICK_LEN - 1));
    // survives a reboot (cfg_init reloads it from NVS)
    bool r, o; CHECK_EQ(cfg_init(&r, &o), ESP_OK);
    CHECK(cfg_cam_nick_get(A, n)); CHECK(strcmp(n, "Cinewhoop") == 0);
    // rename, then clear with ""
    CHECK_EQ(cfg_cam_nick_set(A, "Long range"), ESP_OK);
    CHECK(cfg_cam_nick_get(A, n)); CHECK(strcmp(n, "Long range") == 0);
    CHECK_EQ(cfg_cam_nick_set(A, ""), ESP_OK);
    CHECK(!cfg_cam_nick_get(A, n));
    // forgetting a camera drops its name; the other keeps its own
    cfg_cam_nick_set(A, "A again");
    CHECK_EQ(cfg_forget_camera(A), ESP_OK);
    CHECK(!cfg_cam_nick_get(A, n)); CHECK(cfg_cam_nick_get(B, n));
    CHECK_EQ(cfg_init(&r, &o), ESP_OK); CHECK(!cfg_cam_nick_get(A, n)); CHECK(cfg_cam_nick_get(B, n));
    // all four slots usable, and clearing everything drops all names
    cfg_set_camera_bond("gopro", A, "HERO11", 0); cfg_set_camera_bond("gopro", C3, "HERO9", 0);
    uint8_t D[6] = { 9, 9, 9, 9, 9, 9 };
    cfg_set_camera_bond("gopro", D, "HERO8", 0);
    CHECK_EQ(cfg_cam_nick_set(A, "a"), ESP_OK); CHECK_EQ(cfg_cam_nick_set(C3, "c"), ESP_OK); CHECK_EQ(cfg_cam_nick_set(D, "d"), ESP_OK);
    CHECK(cfg_cam_nick_get(B, n) && cfg_cam_nick_get(A, n) && cfg_cam_nick_get(C3, n) && cfg_cam_nick_get(D, n));
    CHECK_EQ(cfg_clear_camera(), ESP_OK);
    CHECK(!cfg_cam_nick_get(A, n) && !cfg_cam_nick_get(B, n) && !cfg_cam_nick_get(C3, n) && !cfg_cam_nick_get(D, n));
}

int main(void)
{
    RUN(test_profile_pure);
    RUN(test_simple_gopro_zone);
    RUN(test_retired_tokens);
    RUN(test_switch_saves_and_loads);
    RUN(test_edit_inactive_camera);
    RUN(test_no_profile_inherits);
    RUN(test_forget_and_clear);
    RUN(test_rid_any_enabled);
    RUN(test_nicknames);
    TEST_MAIN_END();
}
