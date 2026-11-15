// captive_dns.c -- ~60-line UDP DNS responder (spec 3.1 / 10.2). Answers every
// A query with 10.0.0.1 so OS captive-portal detection opens the page.
//
// Lifecycle: the task owns its socket (a local, never a shared global) and exits on its own when
// captive_dns_stop() clears s_run. captive_dns_start() waits for a previous task to be gone first,
// so a portal reopened within a second can never leave two tasks serving one socket.
#include <string.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cdns";
static volatile bool s_run;      // wanted: the responder should be serving
static volatile bool s_alive;    // a dns_task is still executing (until just before it deletes itself)

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "dns socket/bind failed");
        if (sock >= 0) { close(sock); }
        s_run = false;
        s_alive = false;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };      // also bounds how long stop() takes to be noticed
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) { continue; }                            // timeout / error: re-check s_run

        int qend = 12;
        while (qend < n && buf[qend] != 0) { qend += buf[qend] + 1; }
        qend += 1 + 4;                                       // null label + QTYPE + QCLASS
        if (qend + 16 > (int)sizeof(buf) || qend > n) { continue; }
        uint16_t qtype = (uint16_t)((buf[qend - 4] << 8) | buf[qend - 3]);

        // Response: copy header + question, set QR.
        buf[2] |= 0x80;                                      // QR = response
        buf[3] = 0x00;                                       // RA=0, RCODE=0 (NOERROR)
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;    // NSCOUNT = ARCOUNT = 0

        if (qtype != 1) {
            // Anything but an A query (AAAA, HTTPS, ...): empty NOERROR answer, so the client falls
            // back to IPv4 instead of being handed a bogus record for the wrong type.
            buf[6] = 0; buf[7] = 0;
            sendto(sock, buf, qend, 0, (struct sockaddr *)&from, fl);
            continue;
        }
        buf[6] = 0; buf[7] = 1;                              // ANCOUNT = 1
        uint8_t *a = buf + qend;
        a[0] = 0xC0; a[1] = 0x0C;                            // name pointer to question
        a[2] = 0x00; a[3] = 0x01;                            // TYPE A
        a[4] = 0x00; a[5] = 0x01;                            // CLASS IN
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60;             // TTL 60
        a[10] = 0x00; a[11] = 0x04;                          // RDLENGTH 4
        a[12] = 10; a[13] = 0; a[14] = 0; a[15] = 1;         // portal IP

        sendto(sock, buf, qend + 16, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_alive = false;                                         // last touch of shared state
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (s_run) { return; }
    // A previous task may still be inside its last 1 s receive timeout: wait for it to leave
    // (bounded) so two tasks never share the port.
    for (int i = 0; i < 150 && s_alive; i++) { vTaskDelay(pdMS_TO_TICKS(10)); }
    if (s_alive) { ESP_LOGW(TAG, "previous dns task still running -- not starting another"); return; }
    s_run = true;
    s_alive = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL) != pdPASS) {
        s_run = false;
        s_alive = false;
    }
}

void captive_dns_stop(void)
{
    s_run = false;    // the task notices within its 1 s receive timeout, closes its own socket and exits
}
