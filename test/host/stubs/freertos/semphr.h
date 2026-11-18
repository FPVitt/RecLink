// Host-test stub: single-threaded tests, so the mutex is a no-op.
#pragma once
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { return (SemaphoreHandle_t)b; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { (void)s; (void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
#define portMAX_DELAY 0xFFFFFFFFu
