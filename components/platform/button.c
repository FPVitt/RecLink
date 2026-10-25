// button.c -- single button. Debounced, edge-timed. GPIO9 doubles as the ROM
// download-mode strap on this board, so nothing here samples the button at
// boot -- only button_poll(), called from housekeeping once the app is
// already running (see platform.h).
#include "platform.h"
#include "led_internal.h"

#include "monoclock.h"
#include "sdkconfig.h"
#include "driver/gpio.h"

#define PIN   CONFIG_BRIDGE_BUTTON_GPIO

static inline bool pressed(void)
{
    int lvl = gpio_get_level(PIN);
#if CONFIG_BRIDGE_BUTTON_ACTIVE_LOW
    return lvl == 0;
#else
    return lvl == 1;
#endif
}

void button_hw_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_BRIDGE_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
#else
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
    };
    gpio_config(&io);
}

btn_event_t button_poll(void)
{
    static bool     was = false;
    static uint32_t down_ms = 0;
    static bool     long_fired = false;
    static bool     factory_fired = false;

    bool now = pressed();
    uint32_t t = now_ms();

    if (now && !was) { down_ms = t; long_fired = false; factory_fired = false; }

    btn_event_t ev = BTN_NONE;
    if (now) {
        uint32_t held = t - down_ms;
        if (!factory_fired && held >= 10000) {
            ev = BTN_FACTORY;              // fire once, at the 10 s crossing
            factory_fired = true;
            long_fired = true;             // supersedes the 3 s event
        } else if (!long_fired && held >= 3000) {
            ev = BTN_LONG;                 // fire once, at the 3 s crossing
            long_fired = true;
        }
    } else if (was && !long_fired) {
        uint32_t held = t - down_ms;
        if (held >= 30 && held < 1000) { ev = BTN_SHORT; }
    }
    was = now;
    return ev;
}
