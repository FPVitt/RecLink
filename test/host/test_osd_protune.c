// GoPro Protune OSD tokens (%ix %in %sh %wb %co %sa %ev %pt): each expands to what the camera
// reported, is empty when the camera has not confirmed it (blank-on-unconfirmed, spec 9.4) or when
// its field bit is off, follows the digital filter (lowercase folded up) and the 16-char clamp.
#include "test.h"
#include "osd.h"

uint32_t now_ms(void) { return 1000; }

static void setup(cfg_t *c, fc_state_t *fc, cam_state_t *cam, int digital)
{
    memset(c, 0, sizeof(*c)); memset(fc, 0, sizeof(*fc)); memset(cam, 0, sizeof(*cam));
    c->osd.type = digital ? 1 : 0;
    c->osd.fields_mask = (1u << OSD_TOK_COUNT) - 1u;
    cam->link = CAM_LINK_READY;
}

static void put(val_str_t *s, const char *v) { s->valid = true; strlcpy(s->v, v, sizeof s->v); }

static void fmt(char *out, size_t cap, const char *tmpl, const fc_state_t *fc, const cam_state_t *cam, const cfg_t *c)
{
    osd_format_line(out, cap, tmpl, fc, cam, c, false);
}

static void test_each_token(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam, 0);
    put(&cam.iso_max, "1600"); put(&cam.iso_min, "100"); put(&cam.shutter, "1/120"); put(&cam.wb, "5500K");
    put(&cam.color, "Flat"); put(&cam.sharpness, "Medium"); put(&cam.ev, "-0.5"); put(&cam.protune, "ON");
    char out[32];
    fmt(out, sizeof out, "%ix", &fc, &cam, &c); CHECK(strcmp(out, "1600") == 0);
    fmt(out, sizeof out, "%in", &fc, &cam, &c); CHECK(strcmp(out, "100") == 0);
    fmt(out, sizeof out, "%sh", &fc, &cam, &c); CHECK(strcmp(out, "1/120") == 0);
    fmt(out, sizeof out, "%wb", &fc, &cam, &c); CHECK(strcmp(out, "5500K") == 0);
    fmt(out, sizeof out, "%co", &fc, &cam, &c); CHECK(strcmp(out, "Flat") == 0);
    fmt(out, sizeof out, "%sa", &fc, &cam, &c); CHECK(strcmp(out, "Medium") == 0);
    fmt(out, sizeof out, "%ev", &fc, &cam, &c); CHECK(strcmp(out, "-0.5") == 0);
    fmt(out, sizeof out, "%pt", &fc, &cam, &c); CHECK(strcmp(out, "ON") == 0);
    fmt(out, sizeof out, "%ix-%in %sh", &fc, &cam, &c); CHECK(strcmp(out, "1600-100 1/120") == 0);
}

static void test_blank_when_unconfirmed(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam, 0);
    char out[32];
    // nothing valid: every token renders as nothing, never 0 / -- / a guess
    fmt(out, sizeof out, "[%ix%in%sh%wb%co%sa%ev%pt]", &fc, &cam, &c);
    CHECK(strcmp(out, "[]") == 0);
    put(&cam.shutter, "1/60"); cam.shutter.valid = false;           // a stale string with the flag down
    fmt(out, sizeof out, "[%sh]", &fc, &cam, &c); CHECK(strcmp(out, "[]") == 0);
}

static void test_field_bit(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam, 0);
    put(&cam.wb, "Auto"); put(&cam.ev, "0.0");
    c.osd.fields_mask &= ~(1u << OSD_TOK_WB);
    char out[32];
    fmt(out, sizeof out, "%wb|%ev", &fc, &cam, &c); CHECK(strcmp(out, "|0.0") == 0);
    // the other tokens are not affected by an unrelated bit
    c.osd.fields_mask = 1u << OSD_TOK_EV;
    fmt(out, sizeof out, "%wb|%ev", &fc, &cam, &c); CHECK(strcmp(out, "|0.0") == 0);
}

static void test_digital_and_clamp(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam, 1);
    put(&cam.sharpness, "Medium"); put(&cam.color, "Vibrant"); put(&cam.shutter, "1/120");
    char out[32];
    fmt(out, sizeof out, "%sa %co", &fc, &cam, &c); CHECK(strcmp(out, "MEDIUM VIBRANT") == 0);   // folded up
    fmt(out, sizeof out, "%sh", &fc, &cam, &c); CHECK(strcmp(out, "1/120") == 0);              // '/' is in 0x20..0x5F
    put(&cam.wb, "5500K"); put(&cam.ev, "+2.0");
    fmt(out, sizeof out, "%sa %co %wb %ev", &fc, &cam, &c);                                     // 6+1+7+1+5+1+4 = 25
    CHECK_EQ((int)strlen(out), 16);
    c.osd.type = 0;                                                                             // analog keeps case
    fmt(out, sizeof out, "%sa", &fc, &cam, &c); CHECK(strcmp(out, "Medium") == 0);
}

// %bi: on an analog OSD the Betaflight battery glyph (0x90 full .. 0x96 empty); on a DIGITAL OSD the same
// glyph, patched in after the digital filter (which would otherwise have blanked it and used to leave
// the "#---" bar). Clamped line, field bit and unconfirmed battery still behave.
static void test_battery_icon(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; char out[32];
    for (int digital = 0; digital < 2; digital++) {
        setup(&c, &fc, &cam, digital);
        cam.battery_pct.valid = true;
        cam.battery_pct.v = 100; fmt(out, sizeof out, "%bi", &fc, &cam, &c);
        CHECK_EQ((unsigned char)out[0], 0x90); CHECK_EQ(out[1], 0);
        cam.battery_pct.v = 25;  fmt(out, sizeof out, "%bi%bt", &fc, &cam, &c);
        CHECK_EQ((unsigned char)out[0], 0x90 + (100 - 25) * 7 / 100); CHECK(strcmp(out + 1, "25%") == 0);
        cam.battery_pct.v = 0;   fmt(out, sizeof out, "[%bi]", &fc, &cam, &c);
        CHECK_EQ((unsigned char)out[1], 0x96);
        cam.battery_pct.valid = false; fmt(out, sizeof out, "[%bi]", &fc, &cam, &c); CHECK(strcmp(out, "[]") == 0);
        cam.battery_pct.valid = true; cam.battery_pct.v = 50;
        c.osd.fields_mask &= ~(1u << OSD_TOK_BI); fmt(out, sizeof out, "[%bi]", &fc, &cam, &c); CHECK(strcmp(out, "[]") == 0);
    }
    // digital: a literal '^' elsewhere in the line is not turned into the glyph
    setup(&c, &fc, &cam, 1); cam.battery_pct.valid = true; cam.battery_pct.v = 50;
    fmt(out, sizeof out, "^%bi ^", &fc, &cam, &c);
    CHECK_EQ(out[0], '^'); CHECK((unsigned char)out[1] >= 0x90 && (unsigned char)out[1] <= 0x96); CHECK_EQ(out[2], ' '); CHECK_EQ(out[3], '^');
    // a glyph pushed past the 16-char clamp is dropped, not written outside the line
    fmt(out, sizeof out, "1234567890123456%bi", &fc, &cam, &c);
    CHECK_EQ(strlen(out), 16);
}

// %cm: short camera model from the driver's model string; blank when unknown; follows the field bit.
static void test_cam_model(void)
{
    char o[16];
    struct { const char *in, *want; } k[] = {
        { "HERO11 Black", "GP11" }, { "HERO11 Black Mini", "GP11M" }, { "HERO9 Black", "GP9" }, { "HERO13 Black", "GP13" },
        { "Osmo Action 6", "A6" }, { "Osmo Action 4", "A4" }, { "Osmo Action 5 Pro", "A5P" }, { "Osmo 360", "O360" },
        { "Osmo Pocket 3", "DJI" }, { "", "" }, { "Something", "" },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        osd_cam_model_short(o, sizeof o, k[i].in);
        CHECK(strcmp(o, k[i].want) == 0);
    }
    osd_cam_model_short(o, 3, "HERO11 Black"); CHECK(strcmp(o, "GP") == 0);      // truncates, always terminated
    cfg_t c; fc_state_t fc; cam_state_t cam; char out[32]; setup(&c, &fc, &cam, 1);
    strlcpy(cam.model, "Osmo Action 6", sizeof cam.model);
    fmt(out, sizeof out, "[%cm]", &fc, &cam, &c); CHECK(strcmp(out, "[A6]") == 0);
    cam.model[0] = '\0'; fmt(out, sizeof out, "[%cm]", &fc, &cam, &c); CHECK(strcmp(out, "[]") == 0);   // blank-on-unconfirmed
    strlcpy(cam.model, "HERO11 Black", sizeof cam.model);
    c.osd.fields_mask &= ~(1u << OSD_TOK_CM); fmt(out, sizeof out, "[%cm]", &fc, &cam, &c); CHECK(strcmp(out, "[]") == 0);
    // a retired token is no longer special: it is emitted literally (saved lines are cleaned by cfg_osd_retire_tokens)
    setup(&c, &fc, &cam, 0); fmt(out, sizeof out, "%bb", &fc, &cam, &c); CHECK(strcmp(out, "%b") == 0 || strcmp(out, "%bb") == 0);
}

int main(void)
{
    RUN(test_each_token);
    RUN(test_blank_when_unconfirmed);
    RUN(test_field_bit);
    RUN(test_battery_icon);
    RUN(test_cam_model);
    RUN(test_digital_and_clamp);
    TEST_MAIN_END();
}
