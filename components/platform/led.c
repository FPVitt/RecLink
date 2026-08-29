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

    uint8_t v = 0;      // white channel level
    uint8_t rr = 0, gg = 0, bb = 0;
    switch (p) {
    case LED_BOOT:          v = 255; break;
    case LED_FC_DOWN:       v = nblink(t, 2000, 2, 90, 140); break;
    case LED_CAM_SEARCHING: v = breathe(t, 2600); break;
    case LED_CAM_IDLE:      v = blink(t, 3000, 60); break;
    case LED_RECORDING:     gg = 255; break;
    case LED_CAM_LOST:      v = blink(t, 300, 150); break;
    case LED_PORTAL:        bb = nblink(t, 1000, 2, 70, 110); break;
    case LED_FAULT:         v = nblink(t, 100000, 5, 70, 90); break;
    case LED_IDENTIFY:      v = blink(t, 200, 100); break;
    default:                v = 0; break;
    }
    if (v) { rr = gg = bb = v; }

#if CONFIG_BRIDGE_LED_WS2812
    // cfg.led_brightness scales the WS2812 output (a plain GPIO LED is on/off
    // only and cannot dim, so it is left untouched there).
    const cfg_t *cfg = cfg_get();
    uint8_t scale = cfg ? cfg->led_brightness : 128;
    if (scale != 255) {
        rr = (uint16_t)rr * scale / 255;
        gg = (uint16_t)gg * scale / 255;
        bb = (uint16_t)bb * scale / 255;
    }
#endif
    led_write(rr, gg, bb);
}
