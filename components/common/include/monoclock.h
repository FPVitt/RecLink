// monoclock.h -- monotonic millisecond uptime, used everywhere for timers.
#pragma once
#include <stdint.h>

// Milliseconds since boot. Wraps after ~49 days; all bridge timers are short and
// use unsigned subtraction, which is wrap-safe.
uint32_t now_ms(void);

// 64-bit microseconds since boot (esp_timer). For the F6 clock discipline.
int64_t now_us(void);
