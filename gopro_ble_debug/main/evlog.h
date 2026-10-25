// evlog.h -- the debug event stream the web UI polls (GET /api/events?since=N).
//
// Every BLE packet in or out, every GATT result and every link-level event is
// appended here with a monotonically increasing sequence number. The browser
// does GoPro depacketizing + decoding itself, so this stays a dumb, lossy ring:
// if the UI falls behind, the oldest entries are overwritten and it sees a gap.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EV_SYS = 0,     // text: link state, discovery progress, errors
    EV_TX,          // packet written to `handle` (status = ATT result, 0 ok)
    EV_RX,          // notification received on `handle`
    EV_IND,         // indication received on `handle`
    EV_READ,        // read response from `handle` (status = ATT result)
    EV_ERR,         // text: failed operation
} ev_type_t;

#define EV_DATA_MAX 256

typedef struct {
    uint32_t seq;
    uint32_t ms;
    uint8_t  type;          // ev_type_t
    uint8_t  flags;         // EVF_*
    uint16_t handle;
    int16_t  status;
    uint16_t len;
    uint8_t  data[EV_DATA_MAX];
} ev_t;

#define EVF_KEEPALIVE 0x01  // auto keep-alive traffic (UI can hide it)
#define EVF_TRUNC     0x02  // data longer than EV_DATA_MAX, truncated

void     evlog_init(void);
void     evlog_push(ev_type_t t, uint16_t handle, int16_t status, uint8_t flags,
                    const void *data, size_t len);
void     evlog_text(ev_type_t t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Copy the oldest entry with seq >= since into *out. Returns false if none.
bool     evlog_get(uint32_t since, ev_t *out);
uint32_t evlog_next_seq(void);
uint32_t evlog_oldest_seq(void);

// ESP_LOG mirror (GET /api/log): every log line also lands in a text ring.
void   syslog_hook_init(void);
size_t syslog_dump(char *dst, size_t cap);
