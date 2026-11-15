// Host-test stub: just enough of esp_err.h for the firmware headers to compile off-target.
#pragma once
#include <stddef.h>   // on target esp_err.h pulls this in; several firmware headers rely on it
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND 0x105
