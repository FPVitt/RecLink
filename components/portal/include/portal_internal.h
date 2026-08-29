// portal_internal.h -- shared between portal translation units only.
#pragma once
#include "cfg.h"
#include "cJSON.h"
#include "esp_http_server.h"

// json_cfg.c
cJSON *cfg_to_json(const cfg_t *c);
// Merge present keys from `j` onto *c (partial update, spec 10.4). Returns
// ESP_ERR_INVALID_ARG + message on a bad value.
esp_err_t json_merge_cfg(const cJSON *j, cfg_t *c, char *err, size_t errlen);

// ota.c
esp_err_t ota_stream_begin(void);
esp_err_t ota_stream_write(const void *data, size_t len);
esp_err_t ota_stream_finish(void);   // sets boot partition; caller reboots
void      ota_stream_abort(void);

// portal_task.c
void portal_note_client_activity(void);
