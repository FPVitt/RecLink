#include "fc_state.h"
#include "monoclock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static fc_state_t     s_fc;
static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t  s_mux;

void fc_state_init(void)
{
    s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
    // sane zero state; link_up false, nothing valid
}

void fc_state_get(fc_state_t *dst)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *dst = s_fc;
    xSemaphoreGive(s_mux);
}

void fc_state_locked(fc_state_mut_fn fn, void *ctx)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    fn(&s_fc, ctx);
    xSemaphoreGive(s_mux);
}

void fc_state_publish(const fc_state_t *scratch)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_fc = *scratch;
    xSemaphoreGive(s_mux);
}

bool fc_now_utc_ms(int64_t *out_ms, uint32_t max_age_ms)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool ok = s_fc.rtc_valid;
    int64_t base = (int64_t)s_fc.rtc_epoch * 1000 + s_fc.rtc_millis;
    uint32_t sample = s_fc.rtc_sample_uptime_ms;
    xSemaphoreGive(s_mux);

    if (!ok) { return false; }
    uint32_t age = now_ms() - sample;
    if (max_age_ms && age > max_age_ms) { return false; }
    if (out_ms) { *out_ms = base + (int64_t)age; }
    return true;
}
