// gopro_presets_parse: the camera's video preset list (NotifyPresetStatus protobuf).
#include <stdio.h>
#include <string.h>
#include "gopro_presets.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- tiny protobuf writer ----
typedef struct { uint8_t b[512]; int n; } buf_t;
static void vint(buf_t *o, uint64_t v) { do { uint8_t c = v & 0x7F; v >>= 7; o->b[o->n++] = c | (v ? 0x80 : 0); } while (v); }
static void fv(buf_t *o, int f, uint64_t v) { vint(o, (uint64_t)f << 3); vint(o, v); }
static void fb(buf_t *o, int f, const buf_t *m) { vint(o, ((uint64_t)f << 3) | 2); vint(o, m->n); memcpy(o->b + o->n, m->b, m->n); o->n += m->n; }
static void fs(buf_t *o, int f, const char *s) { buf_t m = {0}; m.n = (int)strlen(s); memcpy(m.b, s, m.n); fb(o, f, &m); }
static buf_t preset(int64_t id, int title, int num, const char *custom, int visible)
{
    buf_t p = {0};
    fv(&p, 1, (uint64_t)id);
    fv(&p, 2, 12);                                  // mode
    if (title >= 0) fv(&p, 3, title);
    if (num) fv(&p, 4, num);
    buf_t st = {0}; fv(&st, 1, 2); fv(&st, 2, 107); fv(&st, 3, 1);
    fb(&p, 7, &st);                                 // a setting_array entry, skipped
    if (custom) fs(&p, 10, custom);
    if (visible >= 0) fv(&p, 11, visible);
    return p;
}

int main(void)
{
    buf_t photo = {0}, video = {0}, msg = {0}, x;
    fv(&photo, 1, 1001);
    x = preset(5, 3, 0, NULL, -1); fb(&photo, 2, &x);
    x = preset(0, 1, 0, NULL, -1);  fb(&video, 2, &x);           // Standard, id 0
    x = preset(1, 0, 0, NULL, 1);   fb(&video, 2, &x);           // Activity, explicitly visible
    x = preset(10, 18, 2, NULL, -1); fb(&video, 2, &x);          // Custom 2
    x = preset(11, 94, 0, "My <FPV> \"x\"", -1); fb(&video, 2, &x);   // custom name, sanitised
    x = preset(12, 2, 0, NULL, 0);  fb(&video, 2, &x);           // hidden: left out
    x = preset(13, 999, 0, NULL, -1); fb(&video, 2, &x);         // unknown title: empty name
    x = preset(0x40000005, 1, 0, NULL, -1); fb(&video, 2, &x);   // big id survives
    fv(&video, 3, 1);                                            // can_add_preset
    fv(&video, 1, 1000);                                         // group id AFTER the presets
    fb(&msg, 1, &photo); fb(&msg, 1, &video);

    cam_preset_t out[CAM_PRESETS_MAX];
    int n = gopro_presets_parse(msg.b, (uint16_t)msg.n, out, CAM_PRESETS_MAX);
    CHECK(n == 6);
    CHECK(out[0].id == 0 && strcmp(out[0].name, "Standard") == 0);
    CHECK(out[1].id == 1 && strcmp(out[1].name, "Activity") == 0);
    CHECK(out[2].id == 10 && strcmp(out[2].name, "Custom 2") == 0);
    CHECK(out[3].id == 11 && strcmp(out[3].name, "My ?FPV? ?x?") == 0);
    CHECK(out[4].id == 13 && out[4].name[0] == '\0');
    CHECK(out[5].id == 0x40000005u);

    CHECK(gopro_presets_parse(msg.b, (uint16_t)msg.n, out, 2) == 2);    // capped
    for (int cut = 1; cut < msg.n; cut++) {                              // truncated: never crashes
        int k = gopro_presets_parse(msg.b, (uint16_t)cut, out, CAM_PRESETS_MAX);
        CHECK(k >= -1 && k <= CAM_PRESETS_MAX);
    }
    uint8_t junk[] = { 0xFF, 0xFF, 0xFF };
    CHECK(gopro_presets_parse(junk, sizeof junk, out, CAM_PRESETS_MAX) == -1);
    CHECK(gopro_presets_parse(junk, 0, out, CAM_PRESETS_MAX) == -1);
    printf("test_gopro_presets.c: %s (%d failed)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
