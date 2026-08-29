#include "cfg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/atomic.h"

static volatile uint32_t s_flags;

void bridge_flag_set(bridge_flag_t f)   { Atomic_OR_u32(&s_flags, (uint32_t)f); }
void bridge_flag_clear(bridge_flag_t f) { Atomic_AND_u32(&s_flags, ~(uint32_t)f); }
bool bridge_flag_get(bridge_flag_t f)   { return (s_flags & (uint32_t)f) != 0; }
uint32_t bridge_flags_all(void)         { return s_flags; }
