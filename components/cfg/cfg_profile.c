// cfg_profile.c -- per-camera settings: pure extract / apply / validate (no NVS, no RTOS, no
// logging) so the host tests compile it directly. Storage and the camera-switch logic are in cfg.c.
#include "cfg.h"
#include "crc.h"

#include <stddef.h>
#include <string.h>

static uint32_t prof_crc(const cfg_profile_t *p)
{
    return crc32_iso_hdlc((const uint8_t *)p, offsetof(cfg_profile_t, crc32));
}

void cfg_profile_extract(const cfg_t *c, cfg_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic   = CFG_PROF_MAGIC;
    p->version = CFG_PROF_VERSION;
    p->size    = sizeof(*p);
    p->record = c->record;       p->preset = c->preset;     p->hilight = c->hilight;
    p->osd = c->osd;             p->clock = c->clock;       p->n_zones = c->n_zones;
    memcpy(p->bands_x, c->bands_x, sizeof(p->bands_x));
    memcpy(p->gp_preset, c->gp_preset, sizeof(p->gp_preset));
    memcpy(p->gp_preset_x, c->gp_preset_x, sizeof(p->gp_preset_x));
    p->rid = c->rid;
    p->crc32 = prof_crc(p);
}

bool cfg_profile_valid(const cfg_profile_t *p)
{
    return p->magic == CFG_PROF_MAGIC && p->version == CFG_PROF_VERSION &&
           p->size == sizeof(*p) && p->crc32 == prof_crc(p);
}

void cfg_osd_retire_tokens(cfg_t *c)
{
    static const char *const k_gone[] = { "%bb", "%cn", "%gs", "%gf", "%fb", "%sp" };
    for (int i = 0; i < 4; i++) {
        char *l = c->osd.line[i];
        l[sizeof(c->osd.line[i]) - 1] = '\0';
        for (size_t t = 0; t < sizeof(k_gone) / sizeof(k_gone[0]); t++) {
            char *at;
            while ((at = strstr(l, k_gone[t])) != NULL) { memmove(at, at + 3, strlen(at + 3) + 1); }
        }
    }
}

bool cfg_profile_apply(cfg_t *c, const cfg_profile_t *p)
{
    if (!cfg_profile_valid(p)) { return false; }
    c->record = p->record;       c->preset = p->preset;     c->hilight = p->hilight;
    c->osd = p->osd;             c->clock = p->clock;       c->n_zones = p->n_zones;
    cfg_osd_retire_tokens(c);
    memcpy(c->bands_x, p->bands_x, sizeof(c->bands_x));
    memcpy(c->gp_preset, p->gp_preset, sizeof(c->gp_preset));
    memcpy(c->gp_preset_x, p->gp_preset_x, sizeof(c->gp_preset_x));
    c->rid = p->rid;
    // The Remote ID tail has its own CRC: a mismatch loads the disabled defaults (spec 8.8.9),
    // exactly as it does for the main blob.
    cfg_rid_check(c);
    return true;
}
