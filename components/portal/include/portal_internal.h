// portal_internal.h -- shared between portal translation units only.
#pragma once
#include "cfg.h"
#include "cJSON.h"
#include "esp_http_server.h"

// json_cfg.c
cJSON *cfg_to_json(const cfg_t *c);

// Streamed JSON object. A cJSON tree costs ~70 B per node plus the print buffer's
// doubling reallocs, so building a whole /api/status (~250 nodes) peaked at 25+ KB
// of a ~35 KB heap -- on top of the TCP buffers still draining the page's assets,
// that exhausted memory and the board stopped answering on a page refresh. Build
// the response one top-level section at a time instead: js_flush() prints every
// member added to `root` so far as `"key":value`, sends it, and frees it. Keys must
// be plain literals (they are written unescaped). Peak memory is one section.
typedef struct {
    httpd_req_t *r;
    bool         first;      // no member written yet (comma handling)
    esp_err_t    err;        // sticky: once a send fails we only free memory
    uint16_t     n;
    char         buf[512];   // coalesces small writes into few chunks
} jstream_t;
void      js_open(jstream_t *s, httpd_req_t *r);               // content-type + "{"
void      js_put(jstream_t *s, const char *d, size_t len);       // raw JSON text (caller keeps it valid)
void      js_flush(jstream_t *s, cJSON *root);                  // emit + free all members of root
esp_err_t js_close(jstream_t *s, cJSON *root);                  // flush, free root, "}" + end of body
esp_err_t cfg_to_json_stream(const cfg_t *c, httpd_req_t *r);   // GET /api/config without the big tree

// Merge present keys from `j` onto *c (partial update, spec 10.4). Returns
// ESP_ERR_INVALID_ARG + message on a bad value.
esp_err_t json_merge_cfg(const cJSON *j, cfg_t *c, char *err, size_t errlen);

// ota.c
esp_err_t ota_stream_begin(size_t total_bytes);
esp_err_t ota_stream_write(const void *data, size_t len);
esp_err_t ota_stream_finish(void);   // sets boot partition; caller reboots
void      ota_stream_abort(void);

// portal_task.c
void portal_note_client_activity(void);
