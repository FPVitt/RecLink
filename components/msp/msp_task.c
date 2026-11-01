// msp_task.c -- sole owner of the FC UART (spec 3.3 / 1.2.3).
//
// 50 Hz tick. Each tick: drain RX -> parse -> publish fc_state; emit debounced
// fc_events; send at most one scheduled read + a bounded number of queued
// commands / OSD slots. Never bursts (spec 9.7 / 12.2).
#include "msp.h"
#include "msp_codec.h"
#include "msp_parse.h"
#include "msp_sched.h"
#include "fc_state.h"

#include <string.h>

#include "cfg.h"
#include "monoclock.h"
#include "applog.h"
#include "platform.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "msp";

#define UART_PORT   CONFIG_BRIDGE_UART_PORT
#define TX_GPIO     CONFIG_BRIDGE_UART_TX_GPIO
#define RX_GPIO     CONFIG_BRIDGE_UART_RX_GPIO
#define UART_RX_BUF 1024
#define UART_TX_BUF 512

#define TICK_MS         20      // 50 Hz
#define CMDS_PER_TICK    3
#define OSD_PER_TICK     4      // all 4 slots per OSD refresh (spec 4.4)

// ---- queues (static) ---------------------------------------------------
typedef struct { uint16_t func; uint16_t len; uint8_t payload[64]; } msp_cmd_t;
typedef struct { uint8_t slot; char text[MSP_TEXT_MAX + 1]; } osd_slot_t;

static StaticQueue_t s_cmdq_s, s_osdq_s, s_evq_s;
static uint8_t s_cmdq_buf[8 * sizeof(msp_cmd_t)];
static uint8_t s_osdq_buf[8 * sizeof(osd_slot_t)];
static uint8_t s_evq_buf[16 * sizeof(fc_event_t)];
static QueueHandle_t s_cmdq, s_osdq, s_evq;

static volatile bool s_refresh_req;

// ---- passenger guard (spec 1.2.1 / 4.6 / 13.1) -----------------------
// The bridge may only issue MSP read requests, MSP2_SET_TEXT, and (opt-in)
// the 4 OSD-placement writes. Anything that could touch arming / RC / the FC
// clock / GPS / DisplayPort / reboot is refused HERE, at the one place that
// owns the UART -- not left to reviewer diligence.
static bool msp_func_forbidden(uint16_t func)
{
    switch (func) {
    case MSP_SET_ARMING_DISABLED:
    case MSP_SET_RAW_RC:
    case MSP_SET_RAW_GPS:
    case MSP_SET_RTC:
    case MSP_DISPLAYPORT:
    case MSP_REBOOT:
        return true;
    default:
        return false;
    }
}

// Allowlist (belt and braces with the denylist above): the only opcodes this
// firmware ever puts on the wire -- the read requests it polls and MSP2_SET_TEXT
// for the OSD slots. A bug cannot make it transmit anything else (SET_MOTOR,
// SET_PID, SET_MODE_RANGE, ...). Extend deliberately if a feature needs more.
static bool msp_func_allowed(uint16_t func)
{
    switch (func) {
    case MSP_API_VERSION:
    case MSP_FC_VARIANT:
    case MSP_STATUS:
    case MSP_RC:
    case MSP_RAW_GPS:
    case MSP_ANALOG:
    case MSP_BOXIDS:
    case MSP_BATTERY_STATE:
    case MSP_OSD_CANVAS:
    case MSP_RTC:
    case MSP2_GET_TEXT:      // craft name (type 2)
    case MSP2_SET_TEXT:
        return true;
    default:
        return false;
    }
}

// ---- public API -------------------------------------------------------
esp_err_t msp_send(uint16_t func, const uint8_t *payload, uint16_t len)
{
    if (msp_func_forbidden(func) || !msp_func_allowed(func)) {
        ESP_LOGE(TAG, "REFUSED MSP func %u (passenger guard)", func);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (len > sizeof(((msp_cmd_t *)0)->payload)) { return ESP_ERR_INVALID_SIZE; }
    msp_cmd_t c = { .func = func, .len = len };
    if (payload && len) { memcpy(c.payload, payload, len); }
    if (xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(5)) != pdTRUE) {   // block 5 ms then drop
        ESP_LOGW(TAG, "msp_cmdq full, dropped func %u", func);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t msp_osd_send_slot(uint8_t slot, const char *text)
{
    if (slot >= OSD_CUSTOM_MSG_COUNT) { return ESP_ERR_INVALID_ARG; }
    osd_slot_t s = { .slot = slot };
    strlcpy(s.text, text ? text : "", sizeof(s.text));
    if (xQueueSend(s_osdq, &s, 0) != pdTRUE) {           // drop-oldest
        osd_slot_t drop;
        xQueueReceive(s_osdq, &drop, 0);
        xQueueSend(s_osdq, &s, 0);
    }
    return ESP_OK;
}

QueueHandle_t msp_fc_events(void) { return s_evq; }
void msp_request_refresh(void) { s_refresh_req = true; }

// ---- helpers ---------------------------------------------------------
static void emit_event(uint8_t type, int32_t value)
{
    fc_event_t e = { .type = type, .value = value };
    // fc_events must never lose arm edges; depth 16 at 50 Hz is ample (spec 3.3).
    if (xQueueSend(s_evq, &e, 0) != pdTRUE) {
        ESP_LOGE(TAG, "fc_events overflow (type %u)", type);
    }
}

static void uart_write_frame(uint16_t func, const uint8_t *payload, uint16_t len)
{
    // Last line of defence: nothing forbidden ever reaches the wire, even if a
    // bug enqueued it or called here directly.
    if (msp_func_forbidden(func) || !msp_func_allowed(func)) {
        ESP_LOGE(TAG, "BLOCKED MSP func %u at TX", func);
        return;
    }
    uint8_t buf[MSP_MAX_PAYLOAD + 16];
    size_t n = msp_encode(buf, sizeof(buf), func, payload, len);
    if (n) { uart_write_bytes(UART_PORT, buf, n); }
}

static void request_read(uint16_t func)
{
    if (func == MSP2_GET_TEXT) {
        uint8_t type = 2;               // craft name (spec 16.A)
        uart_write_frame(func, &type, 1);
    } else {
        uart_write_frame(func, NULL, 0);
    }
}

// ---- the task -------------------------------------------------------
static void msp_task(void *arg)
{
    (void)arg;
    twdt_register(WDT_MSP);

    const cfg_t *cfg = cfg_get();
    uint32_t baud = cfg ? cfg->msp.baud : 115200;

    uart_config_t uc = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uc));
    // Optional pin swap (cfg.msp_swap_pins) for boards/harnesses wired backwards.
    bool swap = cfg && cfg->msp_swap_pins;
    int tx_pin = swap ? RX_GPIO : TX_GPIO;
    int rx_pin = swap ? TX_GPIO : RX_GPIO;
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Max drive strength on TX: some harnesses put a series resistor (board
    // protection, level shifting) between the ESP and the FC's RX pin, which
    // combined with any load on the FC side can sag/slow the edge enough for
    // the FC to misread bytes even though the link "works". Driving harder
    // costs nothing here and can only help.
    (void)gpio_set_drive_capability((gpio_num_t)tx_pin, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "UART%d @ %lu  tx=%d rx=%d%s", UART_PORT, (unsigned long)baud,
             tx_pin, rx_pin, swap ? " (swapped)" : "");

    msp_sched_init();
    msp_rx_t rx;
    msp_rx_reset(&rx);

    fc_state_t st;
    memset(&st, 0, sizeof(st));
    st.arm_bit_index = -1;

    msp_parse_ctx_t pctx;
    msp_parse_ctx_init(&pctx, cfg ? cfg->clock.min_sats : 6);

    // arm edge (spec 4.3): emit on any change, no debounce (owner decision
    // 2026-08-30 -- a spurious edge only toggles the camera, never the aircraft).
    bool armed_pub = false, have_armed = false;

    // measured MSP cadence (diagnostics). EMA of the interval between responses.
    uint32_t last_rc_rx = 0, last_status_rx = 0;

    bool link_pub = false;
    bool gps_good_pub = false;
    bool rtc_valid_pub = false;
    bool ready_emitted = false;

    uint8_t rxb[256];
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        uint32_t now = now_ms();

        // ---- RX -------------------------------------------------------
        int got = uart_read_bytes(UART_PORT, rxb, sizeof(rxb), 0);
        bool published = false;
        for (int i = 0; i < got; i++) {
            msp_frame_t fr;
            bool bad = false;
            if (msp_rx_byte(&rx, rxb[i], &fr, &bad)) {
                st.last_rx_ms = now;
                if (msp_parse_frame(&fr, &st, &pctx)) { /* changed */ }
                if (st.osd_supported && !st.osd_first_ms) { st.osd_first_ms = now; }
                if (fr.dir == '>') {
                    msp_sched_mark_responded(fr.func);
                    // measured cadence: EMA (1/4) of the response interval, dt
                    // capped so a link outage doesn't skew it.
                    if (fr.func == MSP_RC) {
                        if (last_rc_rx) {
                            uint32_t dt = now - last_rc_rx; if (dt > 2000) dt = 2000;
                            st.rc_period_ms = st.rc_period_ms
                                ? (uint16_t)(st.rc_period_ms - (st.rc_period_ms >> 2) + (dt >> 2)) : (uint16_t)dt;
                        }
                        last_rc_rx = now;
                        st.rc_last_rx_ms = now;
                    } else if (fr.func == MSP_STATUS) {
                        if (last_status_rx) {
                            uint32_t dt = now - last_status_rx; if (dt > 2000) dt = 2000;
                            st.status_period_ms = st.status_period_ms
                                ? (uint16_t)(st.status_period_ms - (st.status_period_ms >> 2) + (dt >> 2)) : (uint16_t)dt;
                        }
                        last_status_rx = now;
                    }
                }
                fc_state_publish(&st);
                published = true;
            }
            if (bad) { cfg_stats_bump_msp_crc(); }
        }

        // ---- link health (spec 4.5) --------------------------------
        bool link_up = (now - st.last_rx_ms) < FC_LINK_TIMEOUT_MS && st.last_rx_ms != 0;
        if (link_up != st.link_up) {
            st.link_up = link_up;
            if (link_up && !st.link_up_first_ms) { st.link_up_first_ms = now; }
            fc_state_publish(&st);
            published = true;
        }
        if (link_up != link_pub) {
            link_pub = link_up;
            emit_event(FC_EV_LINK, link_up ? 1 : 0);
            if (!link_up) {
                applog_note("FC link lost");
                msp_sched_rearm_oneshots();     // re-handshake on recovery
                ready_emitted = false;
                // The FC may have rebooted with a different mode order: forget the arm
                // bit and the arm state until BOXIDS + STATUS are decoded again, so a
                // stale bit index can never be applied to the new session's frames and
                // nothing treats "armed" as known in the meantime (spec 13.3).
                pctx.arm_bit_index = -1;
                pctx.boxids_ok = false;
                st.arm_bit_index = -1;
                st.armed_valid = false;
                fc_state_publish(&st);
                published = true;
            }
        }

        // ---- arm edge (spec 4.3, no debounce) --------------------
        if (pctx.arm_bit_index >= 0 && link_up) {
            bool raw = st.armed;
            if (!have_armed || raw != armed_pub) {
                armed_pub = raw;
                have_armed = true;
                emit_event(FC_EV_ARMED, raw ? 1 : 0);
                applog_note("FC %s", raw ? "ARMED" : "DISARMED");
            }
        }

        // ---- GPS-good sustained + RTC-valid edges -----------------
        bool gps_good = st.gps_good;
        if (gps_good != gps_good_pub) {
            gps_good_pub = gps_good;
            emit_event(FC_EV_GPS_GOOD, gps_good ? 1 : 0);
        }
        if (st.rtc_valid != rtc_valid_pub) {
            rtc_valid_pub = st.rtc_valid;
            if (st.rtc_valid) { emit_event(FC_EV_RTC_VALID, 1); }
        }

        // ---- READY (initial handshake complete) -------------------
        if (!ready_emitted && link_up && msp_sched_oneshots_done()) {
            ready_emitted = true;
            emit_event(FC_EV_READY, 1);
            ESP_LOGI(TAG, "FC ready: %s api %u.%u canvas %ux%u craft '%s'",
                     st.fc_variant, st.api_major, st.api_minor,
                     st.osd_cols, st.osd_rows, st.craft_name);
            if (!st.osd_supported) {
                applog_note("FC MSP API %u.%u < 1.47 -- OSD writes disabled",
                            st.api_major, st.api_minor);
            }
        }

        if (s_refresh_req) {
            s_refresh_req = false;
            msp_sched_rearm_oneshots();
        }

        // ---- TX: one scheduled read ------------------------------
        uint16_t func;
        if (msp_sched_pick(now, st.gps_present, st.osd_supported, &func)) {
            request_read(func);
            msp_sched_sent(func, now);
        }

        // ---- TX: OSD slots (only if link up + OSD supported) -----
        if (link_up && st.osd_supported) {
            for (int k = 0; k < OSD_PER_TICK; k++) {
                osd_slot_t s;
                if (xQueueReceive(s_osdq, &s, 0) != pdTRUE) { break; }
                uint8_t p[2 + MSP_TEXT_MAX];
                uint8_t n = (uint8_t)strnlen(s.text, MSP_TEXT_MAX);
                p[0] = MSP2TEXT_CUSTOM_MSG_0 + s.slot;
                if (n == 0) {
                    // A zero-length custom message makes Betaflight fall back to
                    // rendering the element name ("CUSTOM_MESSAGE0"...). Send a
                    // single space so the slot shows blank instead.
                    p[1] = 1;
                    p[2] = ' ';
                    uart_write_frame(MSP2_SET_TEXT, p, 3);
                } else {
                    p[1] = n;
                    memcpy(&p[2], s.text, n);
                    uart_write_frame(MSP2_SET_TEXT, p, 2 + n);
                }
            }
        } else {
            // drain so stale strings don't burst out on recovery
            osd_slot_t drop;
            while (xQueueReceive(s_osdq, &drop, 0) == pdTRUE) { }
        }

        // ---- TX: queued commands --------------------------------
        for (int k = 0; k < CMDS_PER_TICK; k++) {
            msp_cmd_t c;
            if (xQueueReceive(s_cmdq, &c, 0) != pdTRUE) { break; }
            uart_write_frame(c.func, c.len ? c.payload : NULL, c.len);
        }

        (void)published;
        twdt_feed(WDT_MSP);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
    }
}

void msp_start(void)
{
    fc_state_init();
    s_cmdq = xQueueCreateStatic(8, sizeof(msp_cmd_t), s_cmdq_buf, &s_cmdq_s);
    s_osdq = xQueueCreateStatic(8, sizeof(osd_slot_t), s_osdq_buf, &s_osdq_s);
    s_evq  = xQueueCreateStatic(16, sizeof(fc_event_t), s_evq_buf, &s_evq_s);

    static StaticTask_t tcb;
    static StackType_t  stack[4096 / sizeof(StackType_t)];
    xTaskCreateStatic(msp_task, "msp_task", sizeof(stack) / sizeof(stack[0]),
                      NULL, 12, stack, &tcb);
}
