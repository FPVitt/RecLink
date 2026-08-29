// ota.c -- streamed firmware update (spec 10.4 / 11b). The nvs partition is
// untouched, so cfg + NimBLE bond survive; a schema migration runs on next boot
// if CFG_VERSION was bumped.
#include "portal_internal.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "ota";
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static bool s_active;

esp_err_t ota_stream_begin(size_t total)
{
    if (s_active) { return ESP_ERR_INVALID_STATE; }
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) { ESP_LOGE(TAG, "no next OTA partition"); return ESP_FAIL; }
    ESP_LOGI(TAG, "begin -> %s (0x%06lx, %u B), image %u B -- erasing...",
             s_part->label, (unsigned long)s_part->address,
             (unsigned)s_part->size, (unsigned)total);
    // Pass the real size so only the needed sectors are erased (much faster
    // than OTA_SIZE_UNKNOWN, which erases the whole ~1.9 MB partition and can
    // stall the HTTP task long enough for the client to give up).
    esp_err_t e = esp_ota_begin(s_part, total ? total : OTA_SIZE_UNKNOWN, &s_handle);
    if (e == ESP_OK) { s_active = true; ESP_LOGI(TAG, "erased, ready for data"); }
    else { ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e)); }
    return e;
}

esp_err_t ota_stream_write(const void *data, size_t len)
{
    if (!s_active) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = esp_ota_write(s_handle, data, len);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write(%u): %s", (unsigned)len, esp_err_to_name(e)); }
    return e;
}

esp_err_t ota_stream_finish(void)
{
    if (!s_active) { return ESP_ERR_INVALID_STATE; }
    s_active = false;
    esp_err_t e = esp_ota_end(s_handle);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e)); return e; }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) { ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(e)); return e; }
    ESP_LOGW(TAG, "OTA complete -> boot %s next", s_part->label);
    return ESP_OK;
}

void ota_stream_abort(void)
{
    if (!s_active) { return; }
    s_active = false;
    esp_ota_abort(s_handle);
    ESP_LOGW(TAG, "ota aborted");
}

// Called once from app_main after a clean boot: confirms the running image is
// good so the bootloader won't roll it back (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
void portal_ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running image marked valid (%s)", run->label);
    }
}
