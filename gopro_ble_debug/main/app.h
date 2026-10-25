// app.h -- glue between the debug firmware's translation units.
#pragma once
#include "esp_err.h"

// wifi_ap.c
esp_err_t   wifi_ap_start(void);
const char *wifi_ap_ssid(void);
int         wifi_ap_clients(void);
void        captive_dns_start(void);

// web.c
esp_err_t   web_start(void);

// app_main.c
void        app_mark_valid(void);    // cancel bootloader rollback for this image
