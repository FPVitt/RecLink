// Minimal test helpers for the host tests (no framework, no ESP-IDF).
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

static int g_fails, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fails++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; \
    if (_a != _b) { printf("  FAIL %s:%d: %s == %s  (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fails++; } } while (0)
#define RUN(fn) do { int _f = g_fails; fn(); printf("%-34s %s\n", #fn, g_fails == _f ? "ok" : "FAILED"); } while (0)
#define TEST_MAIN_END() do { printf("%s: %d checks, %d failed\n", __FILE__, g_checks, g_fails); return g_fails ? 1 : 0; } while (0)
