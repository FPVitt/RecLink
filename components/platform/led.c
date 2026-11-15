// led.c -- status LED. Supports a single WS2812 pixel (RMT) or a plain GPIO LED.
// housekeeping calls led_service() at >=5 Hz; the pattern engine is a small state
// table driven by now_ms(). No blocking, no allocation.
#include "platform.h"
#include "led_internal.h"

#include "monoclock.h"
#include "cfg.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#if CONFIG_BRIDGE_LED_WS2812
#include "driver/rmt_tx.h"
#include "led_strip.h"
#endif

static led_pattern_t s_base = LED_BOOT;
static uint32_t      s_identify_until;

#if CONFIG_BRIDGE_LED_WS2812
static led_strip_handle_t s_strip;
#endif

void led_hw_init(void)
{
#if CONFIG_BRIDGE_LED_WS2812
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_BRIDGE_LED_GPIO_NUM,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rc = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) == ESP_OK) {
        led_strip_clear(s_strip);
    }
#else
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_LED_GPIO_NUM,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
#endif
}

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_BRIDGE_LED_WS2812
    if (!s_strip) { return; }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
#else
    bool on = (r | g | b) != 0;
#if CONFIG_BRIDGE_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_BRIDGE_LED_GPIO_NUM, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_BRIDGE_LED_GPIO_NUM, on ? 1 : 0);
#endif
#endif
}

// Returns brightness 0..255 for a blink/breathe pattern at time t (ms).
static uint8_t blink(uint32_t t, uint32_t period, uint32_t on_ms)
{
    return (t % period) < on_ms ? 255 : 0;
}

static uint8_t nblink(uint32_t t, uint32_t period, uint32_t n, uint32_t on_ms, uint32_t gap_ms)
{
    uint32_t burst = n * (on_ms + gap_ms);
    uint32_t p = t % period;
    if (p >= burst) { return 0; }
    return (p % (on_ms + gap_ms)) < on_ms ? 255 : 0;
}

static uint8_t breathe(uint32_t t, uint32_t period)
{
    uint32_t p = t % period;
    uint32_t half = period / 2;
    uint32_t up = p < half ? p : period - p;
    return (uint8_t)(up * 255 / half);
}

void led_request(led_pattern_t p) { s_base = p; }

void led_identify(uint32_t ms) { s_identify_until = now_ms() + ms; }

void led_service(void)
{
    uint32_t t = now_ms();
    led_pattern_t p = s_base;
    if ((int32_t)(s_identify_until - t) > 0) { p = LED_IDENTIFY; }

    // Pattern timing (identical on every board): v is the on/off/breathe level 0..255.
    uint8_t v = 0;
    switch (p) {
    case LED_BOOT:          v = 255; break;
    case LED_FC_DOWN:       v = nblink(t, 2000, 2, 90, 140); break;
    case LED_CAM_SEARCHING: v = breathe(t, 2600); break;
    case LED_CAM_IDLE:      v = blink(t, 3000, 60); break;
    case LED_RECORDING:     v = 255; break;
    case LED_CAM_LOST:      v = blink(t, 300, 150); break;
    case LED_PORTAL:        v = nblink(t, 1000, 2, 70, 110); break;
    case LED_FAULT:         v = nblink(t, 100000, 5, 70, 90); break;
    case LED_IDENTIFY:      v = blink(t, 200, 100); break;
    default:                v = 0; break;
    }

#if CONFIG_BRIDGE_LED_WS2812
    // An RGB pixel adds a colour per state on top of the same timing (spec 10.6), and
    // cfg.led_brightness scales it (a plain GPIO LED is on/off only and cannot dim).
    static const uint8_t colour[LED_PATTERN_COUNT][3] = {
        [LED_BOOT]          = {255, 255, 255},   // white
        [LED_FC_DOWN]       = {170,   0, 255},   // purple
        [LED_CAM_SEARCHING] = {255, 130,   0},   // amber
        [LED_CAM_IDLE]      = {  0, 255, 255},   // cyan
        [LED_RECORDING]     = {  0, 255,   0},   // green
        [LED_CAM_LOST]      = {255,   0,   0},   // red
        [LED_PORTAL]        = {  0,   0, 255},   // blue
        [LED_FAULT]         = {255,   0,   0},   // red
        [LED_IDENTIFY]      = {255, 255, 255},   // white
    };
    const cfg_t *cfg = cfg_get();
    uint32_t scale = cfg ? cfg->led_brightness : 128;
    const uint8_t *c = colour[p < LED_PATTERN_COUNT ? p : LED_BOOT];   // v is 0 for any unknown pattern
    uint32_t k = (uint32_t)v * scale;            // 0..255*255
    uint8_t rr = (uint8_t)(c[0] * k / 65025);
    uint8_t gg = (uint8_t)(c[1] * k / 65025);
    uint8_t bb = (uint8_t)(c[2] * k / 65025);
    // Skip identical frames (one RMT transfer per change) but still refresh once a
    // second so a glitched pixel cannot stay wrong.
    static uint8_t last[3];
    static uint32_t last_ms;
    if (rr == last[0] && gg == last[1] && bb == last[2] && (uint32_t)(t - last_ms) < 1000) { return; }
    last[0] = rr; last[1] = gg; last[2] = bb; last_ms = t;
    led_write(rr, gg, bb);
#else
    led_write(v, v, v);
#endif
}
