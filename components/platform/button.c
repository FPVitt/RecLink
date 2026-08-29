// button.c -- single button. Debounced, edge-timed. Boot-time hold is sampled
// once in button_hw_init() so app_main can branch on it before tasks start.
#include "platform.h"
#include "led_internal.h"

#include "monoclock.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN   CONFIG_BRIDGE_BUTTON_GPIO

static bool s_boot_long;
static bool s_boot_factory;

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

    // Sample the boot-time hold. Blocking here is fine: no tasks exist yet and
    // the worst case is a 10 s wait only when the user is deliberately holding.
    if (!pressed()) { return; }
    uint32_t start = now_ms();
    while (pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t held = now_ms() - start;
        if (held >= 10000) { s_boot_factory = true; s_boot_long = true; break; }
        if (held >= 3000)  { s_boot_long = true; }
    }
}

bool button_held_at_boot_long(void)    { return s_boot_long; }
bool button_held_at_boot_factory(void) { return s_boot_factory; }

btn_event_t button_poll(void)
{
    static bool     was = false;
    static uint32_t down_ms = 0;
    static bool     consumed = false;

    bool now = pressed();
    uint32_t t = now_ms();

    if (now && !was) { down_ms = t; consumed = false; }

    btn_event_t ev = BTN_NONE;
    if (now && !consumed && (t - down_ms) >= 3000) {
        ev = BTN_LONG;          // fire on threshold cross, once
        consumed = true;
    }
    if (!now && was && !consumed) {
        if ((t - down_ms) >= 30 && (t - down_ms) < 1000) { ev = BTN_SHORT; }
        consumed = true;
    }
    was = now;
    return ev;
}
