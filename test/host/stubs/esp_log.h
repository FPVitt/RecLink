// Host-test stub: logging compiles away (set -DHOST_LOG=1 to print it).
#pragma once
#include <stdio.h>
#ifdef HOST_LOG
#define ESP_LOGI(t, f, ...) printf("I %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) printf("W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, f, ...) printf("E %s: " f "\n", t, ##__VA_ARGS__)
#else
#define ESP_LOGI(t, f, ...) ((void)(t))
#define ESP_LOGW(t, f, ...) ((void)(t))
#define ESP_LOGE(t, f, ...) ((void)(t))
#endif
#define ESP_LOGD(t, f, ...) ((void)(t))
