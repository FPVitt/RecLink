#include "cfg.h"

// Plain lock-free word: set/clear from many tasks, read from the portal.
// GCC __atomic builtins avoid pulling freertos/atomic.h (which emits a wall of
// unused-static-function warnings).
static volatile uint32_t s_flags;

void bridge_flag_set(bridge_flag_t f)   { __atomic_fetch_or(&s_flags, (uint32_t)f, __ATOMIC_RELAXED); }
void bridge_flag_clear(bridge_flag_t f) { __atomic_fetch_and(&s_flags, ~(uint32_t)f, __ATOMIC_RELAXED); }
bool bridge_flag_get(bridge_flag_t f)   { return (__atomic_load_n(&s_flags, __ATOMIC_RELAXED) & (uint32_t)f) != 0; }
uint32_t bridge_flags_all(void)         { return __atomic_load_n(&s_flags, __ATOMIC_RELAXED); }
