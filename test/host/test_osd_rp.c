// %rp in the OSD formatter (spec 8.8.7 / 9.1): the arrow placeholder is patched in AFTER the
// digital filter and the 16-char clamp (so 0x60..0x6F survive on digital OSD and nothing else
// outside 0x20..0x5F does), it is dropped if the clamp cut it, and the field-enable bit works.
#include "test.h"
#include "osd.h"
#include "rid_core.h"

uint32_t now_ms(void) { return 1000; }

static void setup(cfg_t *c, fc_state_t *fc, cam_state_t *cam)
{
    memset(c, 0, sizeof(*c)); memset(fc, 0, sizeof(*fc)); memset(cam, 0, sizeof(*cam));
    c->osd.type = 1;                                     // digital
    c->osd.fields_mask = 0xFFFFFFFFu;
    cam->link = CAM_LINK_READY;
}

static osd_rp_t make_rp(uint16_t brg, bool hdg)
{
    osd_rp_t rp; memset(&rp, 0, sizeof rp);
    rid_render_in_t in = { RID_R_PROX, RID_LVL_WARN, 1, true, 240, brg, hdg, 0, RID_BRG_AUTO, false, RID_VERT_UNKNOWN };
    rid_render(rp.text, sizeof rp.text, &in, &rp.pt);
    return rp;
}

// The vertical indicator is plain '+' / '-' after the (patched) bearing arrow.
static void test_vert_glyph_survives_filter(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam;
    memset(&c, 0, sizeof c); memset(&fc, 0, sizeof fc); memset(&cam, 0, sizeof cam);
    c.osd.type = 1; c.osd.fields_mask = 0xFFFFFFFFu; cam.link = CAM_LINK_READY;
    osd_rp_t rp; memset(&rp, 0, sizeof rp);
    rid_render_in_t in = { RID_R_PROX, RID_LVL_WARN, 1, true, 240, 90, true, 0, RID_BRG_AUTO, false, RID_VERT_ABOVE };
    rid_render(rp.text, sizeof rp.text, &in, &rp.pt);
    char out[20];
    int n = osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp);
    CHECK_EQ(n, 13); CHECK_EQ((uint8_t)out[11], 0x64); CHECK_EQ(out[12], '+');   // '+' is plain text
    in.vert = RID_VERT_BELOW; in.hdg_valid = false;                 // compass text: '-'
    rid_render(rp.text, sizeof rp.text, &in, &rp.pt);
    osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp);
    CHECK(strcmp(out, "!RID1 240M E-") == 0);
}

static void test_arrow_survives_digital_filter(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam);
    osd_rp_t rp = make_rp(90, true);
    char out[20];
    int n = osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp);
    CHECK_EQ(n, 12);
    CHECK(memcmp(out, "!RID1 240M ", 11) == 0);
    CHECK_EQ((uint8_t)out[11], 0x64);                    // EAST arrow, not 'D' or ' '
    for (int i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)out[i];
        if (i != 11) { CHECK(ch >= 0x20 && ch <= 0x5F); }
    }
    // the same on analog
    c.osd.type = 0;
    osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp);
    CHECK_EQ((uint8_t)out[11], 0x64);
}

static void test_arrow_cut_by_clamp(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam);
    osd_rp_t rp = make_rp(0, true);
    char out[20];
    // 7-char prefix + 12 = 19 > 16: the arrow (index 18) is cut, never patched past the end
    int n = osd_format_line_rp(out, sizeof out, "ABCDEF %rp", &fc, &cam, &c, false, &rp);
    CHECK_EQ(n, 16);
    for (int i = 0; i < n; i++) { CHECK((uint8_t)out[i] >= 0x20 && (uint8_t)out[i] <= 0x5F); }
    // 4-char prefix: arrow at 15 survives
    n = osd_format_line_rp(out, sizeof out, "ABC %rp", &fc, &cam, &c, false, &rp);
    CHECK_EQ(n, 16); CHECK_EQ((uint8_t)out[15], 0x68);
}

static void test_compass_fallback_and_field_off(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam);
    osd_rp_t rp = make_rp(180, false);                   // no valid heading
    char out[20];
    osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp);
    CHECK(strcmp(out, "!RID1 240M S") == 0);
    c.osd.fields_mask &= ~(1u << OSD_TOK_RP);
    CHECK_EQ(osd_format_line_rp(out, sizeof out, "%rp", &fc, &cam, &c, false, &rp), 0);
    // NULL rp / the plain formatter: %rp is empty
    c.osd.fields_mask = 0xFFFFFFFFu;
    CHECK_EQ(osd_format_line(out, sizeof out, "%rp", &fc, &cam, &c, false), 0);
}

// ---- the line containing %rp is replaced (no separate takeover slot) ---------------------------
static osd_rp_t rp_text(const char *t, bool replace)
{
    osd_rp_t rp; memset(&rp, 0, sizeof rp);
    snprintf(rp.text, sizeof rp.text, "%s", t);
    rp.pt.idx[0] = rp.pt.idx[1] = -1;
    rp.replace = replace;
    return rp;
}

static void test_line_replacement(void)
{
    cfg_t c; fc_state_t fc; cam_state_t cam; setup(&c, &fc, &cam);
    strcpy(c.osd.line[0], "AA"); strcpy(c.osd.line[1], "BB %rp"); strcpy(c.osd.line[2], "CC %rp");
    strcpy(c.osd.line[3], "DD");
    char l[4][20];
    CHECK_EQ(osd_rp_line(&c), 1);
    // below the threshold: inline on the FIRST %rp line only
    osd_rp_t rp = rp_text("RID 820M N", false);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    CHECK(strcmp(l[0], "AA") == 0); CHECK(strcmp(l[1], "BB RID 820M N") == 0);
    CHECK(strcmp(l[2], "CC ") == 0); CHECK(strcmp(l[3], "DD") == 0);
    // at / above the threshold (and the fail text): the whole line is replaced
    rp = rp_text("!!RID 80M ^", true); rp.pt.idx[0] = 10; rp.pt.glyph[0] = 0x68;
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    CHECK(strcmp(l[1], "!!RID 80M \x68") == 0); CHECK(strcmp(l[0], "AA") == 0);
    rp = rp_text("RID RX FAIL", true);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    CHECK(strcmp(l[1], "RID RX FAIL") == 0);
    // blink-off phase: the line is empty, not the layout
    rp = rp_text("", true);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp); CHECK_EQ(l[1][0], 0);
    // CAM LOST: the layout is blanked, the Remote ID message still shows on its line
    cam.link = CAM_LINK_LOST;
    rp = rp_text("!!RID 80M N", true);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    CHECK(strcmp(l[0], "CAM LOST") == 0); CHECK(strcmp(l[1], "!!RID 80M N") == 0); CHECK_EQ(l[2][0], 0);
    rp = rp_text("RID 820M N", false);                    // inline NOTICE: not shown during CAM LOST
    osd_compose_lines(l, &fc, &cam, &c, false, &rp); CHECK_EQ(l[1][0], 0);
    // %rp on line 0 during CAM LOST: the Remote ID message wins that line
    strcpy(c.osd.line[0], "%rp"); rp = rp_text("RID RX FAIL", true);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp); CHECK(strcmp(l[0], "RID RX FAIL") == 0);
    // no line contains %rp: nothing from Remote ID anywhere (no invented slot)
    cam.link = CAM_LINK_READY;
    strcpy(c.osd.line[0], "AA"); strcpy(c.osd.line[1], "BB"); strcpy(c.osd.line[2], "CC");
    CHECK_EQ(osd_rp_line(&c), -1);
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    for (int i = 0; i < 4; i++) { CHECK(strstr(l[i], "RID") == NULL); }
    cam.link = CAM_LINK_LOST;
    osd_compose_lines(l, &fc, &cam, &c, false, &rp);
    for (int i = 0; i < 4; i++) { CHECK(strstr(l[i], "RID") == NULL); }
}

int main(void)
{
    RUN(test_arrow_survives_digital_filter);
    RUN(test_arrow_cut_by_clamp);
    RUN(test_compass_fallback_and_field_off);
    RUN(test_vert_glyph_survives_filter);
    RUN(test_line_replacement);
    TEST_MAIN_END();
}
