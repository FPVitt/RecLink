// cfg_defaults.c -- the factory blob. Values come straight from spec 8/9/11.
#include "cfg.h"
#include <string.h>

void cfg_load_defaults(cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    d->magic   = CFG_MAGIC;
    d->version = CFG_VERSION;
    d->size    = sizeof(cfg_t);

    // camera: unbonded
    d->camera.brand[0] = '\0';
    d->camera.bonded   = 0;

    // record (spec 8.1)
    d->record.mode              = REC_ARM;
    d->record.arm_delay_ms      = 300;
    d->record.stop_delay_ms     = 1500;
    d->record.verify_timeout_ms = 800;
    d->record.retry_count       = 3;
    d->record.sw.channel        = 0;      // none
    d->record.sw.threshold_us   = 1700;

    // preset (spec 8.3) -- 3-pos switch band edges
    d->preset.enabled             = 0;
    d->preset.channel             = 0;
    d->preset.dwell_ms            = 400;
    d->preset.allow_during_record = 0;
    d->preset.bands[0].max_us = 1300; d->preset.bands[0].id = 0x00000000u;
    d->preset.bands[1].max_us = 1700; d->preset.bands[1].id = 0x00000001u;
    d->preset.bands[2].max_us = 2100; d->preset.bands[2].id = 0x00000002u;   // top zone, fixed edge

    // osd (spec 9.2 default layout)
    d->osd.refresh_hz  = 4;
    d->osd.type        = 0;      // analog / full character set
    d->osd.corner      = 1;
    strcpy(d->osd.line[0], "%st %et");
    strcpy(d->osd.line[1], "BAT %bt %wr");
    strcpy(d->osd.line[2], "%rs%fp %ln");
    strcpy(d->osd.line[3], "SD %sd");
    d->osd.fields_mask = (1u << OSD_TOK_COUNT) - 1u;   // all tokens on
    d->osd.warn.batt_pct     = 15;
    d->osd.warn.sd_low_min   = 3;
    d->osd.warn.blink        = 1;
    d->osd.warn.show_on_lost = 1;
    d->osd.units = 0;

    // clock / F6 (spec 8.7 / 11.1)
    d->clock.enabled           = 1;
    d->clock.require_gps       = 1;
    d->clock.min_sats          = 6;
    d->clock.threshold_s       = 10;
    d->clock.utc_offset_min    = 0;
    d->clock.dst               = 0;
    d->clock.check_period_min  = 5;
    d->clock.resync_period_min = 60;
    d->clock.retry_count       = 2;

    // gps position push (DJI)
    d->gps_push.enabled = 0;
    d->gps_push.rate_hz = 1;

    d->hilight.channel = 0;

    d->msp.baud = 115200;
    d->msp_swap_pins = 0;       // board wired TX=BRIDGE_UART_TX_GPIO, RX=..._RX_GPIO
    d->ui_advanced   = 0;       // portal starts in basic mode

    // wifi
    d->wifi.ssid[0]           = '\0';    // auto: RECLINK-XXXX
    strcpy(d->wifi.password, "reclink-fpv");  // >= 8 chars; user must change
    d->wifi.channel           = 0;
    d->wifi.portal_trigger    = PORTAL_TRIG_BUTTON;
    d->wifi.portal_timeout_min = 15;

    d->led_brightness = 128;
    d->crc32 = 0;   // filled by cfg_commit / cfg_init
}
