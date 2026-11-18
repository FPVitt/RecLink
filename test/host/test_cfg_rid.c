// cfg v7 Remote ID tail (spec 8.8.9 / 11.1): factory defaults, migrate_6_to_7 (real defaults with
// enabled = 0, %rp bit set ONCE), cfg_validate never OR-ing OSD_TOK_RP back in, the tail's own
// rid_crc32 (a mismatch -> disabled defaults, through the real cfg_init load path), validation.
#include "test.h"
#include "cfg.h"
#include "crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stddef.h>

bool cfg_migrate_chain(cfg_t *c);   // cfg_migrate.c

// ---- in-RAM NVS (one blob) ---------------------------------------------------------------
static uint8_t g_blob[sizeof(cfg_t) + 64];
static size_t  g_blob_len;
static int     g_writes;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { g_blob_len = 0; return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{ (void)h; (void)k; memcpy(g_blob, v, len); g_blob_len = len; g_writes++; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h; (void)k;
    if (!g_blob_len) { return ESP_ERR_NOT_FOUND; }
    size_t n = g_blob_len < *len ? g_blob_len : *len;
    memcpy(out, g_blob, n); *len = g_blob_len;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; g_blob_len = 0; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static uint32_t head_crc(const cfg_t *c) { return crc32_iso_hdlc((const uint8_t *)c, offsetof(cfg_t, crc32)); }

static void check_rid_defaults(const cfg_t *c)
{
    CHECK_EQ(c->rid.enabled, 0); CHECK_EQ(c->rid.mode, RID_MODE_BLE); CHECK_EQ(c->rid.ble_duty, RID_DUTY_NORMAL);
    CHECK_EQ(c->rid.ble_coded, 0); CHECK_EQ(c->rid.stale_s, 5); CHECK_EQ(c->rid.min_sats, 6);
    CHECK_EQ(c->rid.warn.enabled, 1); CHECK_EQ(c->rid.warn.notice_m, 1000); CHECK_EQ(c->rid.warn.warn_m, 300);
    CHECK_EQ(c->rid.warn.alert_m, 100); CHECK_EQ(c->rid.warn.blink, 1); CHECK_EQ(c->rid.warn.takeover, 1);
    CHECK_EQ(c->rid.warn.takeover_slot, 1); CHECK_EQ(c->rid.warn.takeover_level, 2);
    CHECK_EQ(c->rid.warn.bearing, 0); CHECK_EQ(c->rid.warn.ground, 0);
    for (int i = 0; i < RID_IGNORE_MAX; i++) { CHECK_EQ(c->rid.ignore[i][0], 0); }
}

static void test_defaults(void)
{
    cfg_t c; cfg_load_defaults(&c);
    check_rid_defaults(&c);
    CHECK(c.osd.fields_mask & (1u << OSD_TOK_RP));
    CHECK(cfg_rid_check(&c));                                      // sealed
    CHECK_EQ(CFG_VERSION, 7);
    CHECK(OSD_TOK_RP == OSD_TOK_BI + 1); CHECK(OSD_TOK_COUNT <= 32);
}

static void test_migrate_6_to_7(void)
{
    cfg_t c; cfg_load_defaults(&c);
    c.version = 6;
    memset(&c.rid, 0, sizeof(c.rid));                              // what cfg_init leaves for a v6 blob
    c.osd.fields_mask = 0x0FFFu;                                    // classic tokens only
    CHECK(cfg_migrate_chain(&c));
    CHECK_EQ(c.version, 7); CHECK_EQ(c.size, sizeof(cfg_t));
    check_rid_defaults(&c);                                        // NOT a zero-fill
    CHECK(c.osd.fields_mask & (1u << OSD_TOK_RP));
    CHECK(cfg_rid_check(&c));
    // the whole chain from v1 lands on the same defaults
    cfg_t d; cfg_load_defaults(&d); d.version = 1;
    memset((uint8_t *)&d + offsetof(cfg_t, crc32) + 4, 0, sizeof(d) - offsetof(cfg_t, crc32) - 4);
    CHECK(cfg_migrate_chain(&d)); CHECK_EQ(d.version, 7); check_rid_defaults(&d);
}

static void test_validate_no_or_in(void)
{
    // A user switched %rp off while every classic token is on: validate must keep it off.
    cfg_t c; cfg_load_defaults(&c);
    c.osd.fields_mask &= ~(1u << OSD_TOK_RP);
    char err[64];
    CHECK_EQ(cfg_validate(&c, err, sizeof err), ESP_OK);
    CHECK(!(c.osd.fields_mask & (1u << OSD_TOK_RP)));
    CHECK(c.osd.fields_mask & (1u << OSD_TOK_BI));                 // older appended tokens still OR-ed in
    c.osd.fields_mask = 0x0FFFu;
    CHECK_EQ(cfg_validate(&c, err, sizeof err), ESP_OK);
    CHECK_EQ(c.osd.fields_mask, (1u << OSD_TOK_RP) - 1u);
}

static void test_rid_validate(void)
{
    cfg_t c; cfg_load_defaults(&c);
    c.rid.enabled = 7; c.rid.mode = 9; c.rid.ble_duty = 9; c.rid.ble_coded = 1;
    c.rid.stale_s = 99; c.rid.min_sats = 3;
    c.rid.warn.takeover_slot = 9; c.rid.warn.takeover_level = 0; c.rid.warn.bearing = 8;
    c.rid.warn.notice_m = 10; c.rid.warn.warn_m = 60000; c.rid.warn.alert_m = 400;
    cfg_rid_validate(&c);
    CHECK_EQ(c.rid.enabled, 1); CHECK_EQ(c.rid.mode, 2); CHECK_EQ(c.rid.ble_duty, 2); CHECK_EQ(c.rid.ble_coded, 0);
    CHECK_EQ(c.rid.stale_s, 30); CHECK_EQ(c.rid.min_sats, 5);
    CHECK_EQ(c.rid.warn.takeover_slot, 3); CHECK_EQ(c.rid.warn.takeover_level, 1); CHECK_EQ(c.rid.warn.bearing, 3);
    CHECK_EQ(c.rid.warn.notice_m, 5000); CHECK_EQ(c.rid.warn.warn_m, 400); CHECK_EQ(c.rid.warn.alert_m, 20);
    // LOW duty forces stale_s >= 10
    c.rid.ble_duty = RID_DUTY_LOW; c.rid.stale_s = 4; cfg_rid_validate(&c); CHECK_EQ(c.rid.stale_s, 10);
    c.rid.ble_duty = RID_DUTY_NORMAL; c.rid.stale_s = 1; cfg_rid_validate(&c); CHECK_EQ(c.rid.stale_s, 2);
    c.rid.min_sats = 13; cfg_rid_validate(&c); CHECK_EQ(c.rid.min_sats, 12);
    // ignore list: printable, NUL-terminated, de-duplicated, compacted
    memset(c.rid.ignore, 0, sizeof(c.rid.ignore));
    strcpy(c.rid.ignore[0], "");  strcpy(c.rid.ignore[1], "MYDRONE1");
    strcpy(c.rid.ignore[2], "MYDRONE1"); memset(c.rid.ignore[3], 'X', RID_ID_LEN);   // unterminated
    cfg_rid_validate(&c);
    CHECK(strcmp(c.rid.ignore[0], "MYDRONE1") == 0);
    CHECK_EQ(strlen(c.rid.ignore[1]), 20u);                         // forced NUL at [20]
    CHECK_EQ(c.rid.ignore[2][0], 0); CHECK_EQ(c.rid.ignore[3][0], 0);
    c.rid.ignore[0][2] = 0x07; cfg_rid_validate(&c);                // control char -> cleared
    CHECK_EQ(strlen(c.rid.ignore[0]), 20u); CHECK_EQ(c.rid.ignore[1][0], 0);
}

static void test_rid_crc_fallback(void)
{
    // Through the real load path: a v7 blob whose main CRC is good but whose rid tail was flipped
    // to enabled=1, mode=BOTH must come back DISABLED (defaults) and be re-committed.
    cfg_t c; cfg_load_defaults(&c);
    c.rid.enabled = 1; c.rid.mode = RID_MODE_BLE; c.rid.warn.alert_m = 150;
    cfg_rid_seal(&c);
    c.crc32 = head_crc(&c);
    memcpy(g_blob, &c, sizeof c); g_blob_len = sizeof c;
    bool reset = false, orphan = false;
    CHECK_EQ(cfg_init(&reset, &orphan), ESP_OK);
    CHECK(!reset);
    CHECK_EQ(cfg_get()->rid.enabled, 1); CHECK_EQ(cfg_get()->rid.warn.alert_m, 150);   // intact tail kept

    c.rid.mode = RID_MODE_BOTH;                                     // bit-flip, CRC not updated
    memcpy(g_blob, &c, sizeof c); g_blob_len = sizeof c;
    int w0 = g_writes;
    CHECK_EQ(cfg_init(&reset, &orphan), ESP_OK);
    CHECK(!reset);                                                 // the rest of the config survives
    check_rid_defaults(cfg_get());
    CHECK(g_writes > w0);                                          // re-committed
    cfg_t back; memcpy(&back, g_blob, sizeof back);
    CHECK(cfg_rid_check(&back)); CHECK_EQ(back.rid.enabled, 0);

    // a v6 blob (shorter, no tail) migrates through cfg_init
    cfg_t v6; cfg_load_defaults(&v6); v6.version = 6; v6.osd.fields_mask = 0x0FFFu;
    v6.size = (uint16_t)offsetof(cfg_t, rid);
    v6.crc32 = head_crc(&v6);
    memcpy(g_blob, &v6, offsetof(cfg_t, rid)); g_blob_len = offsetof(cfg_t, rid);
    CHECK_EQ(cfg_init(&reset, &orphan), ESP_OK);
    CHECK_EQ(cfg_get()->version, 7); check_rid_defaults(cfg_get());
    CHECK(cfg_get()->osd.fields_mask & (1u << OSD_TOK_RP));
    CHECK_EQ(g_blob_len, sizeof(cfg_t));
}

static void test_commit_seals(void)
{
    cfg_t c; cfg_snapshot(&c);
    c.rid.enabled = 1; c.rid.warn.notice_m = 2000;
    CHECK_EQ(cfg_commit(&c), ESP_OK);
    cfg_t back; memcpy(&back, g_blob, sizeof back);
    CHECK(cfg_rid_check(&back)); CHECK_EQ(back.rid.warn.notice_m, 2000);
}

int main(void)
{
    RUN(test_defaults);
    RUN(test_migrate_6_to_7);
    RUN(test_validate_no_or_in);
    RUN(test_rid_validate);
    RUN(test_rid_crc_fallback);
    RUN(test_commit_seals);
    TEST_MAIN_END();
}
