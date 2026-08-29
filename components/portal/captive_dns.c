// captive_dns.c -- ~60-line UDP DNS responder (spec 3.1 / 10.2). Answers every
// A query with 192.168.4.1 so OS captive-portal detection opens the page.
#include <string.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cdns";
static TaskHandle_t s_task;
static int s_sock = -1;

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "dns socket/bind failed");
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            if (s_sock < 0) { break; }
            continue;
        }
        // Build a minimal response: copy header+question, set QR, 1 answer.
        buf[2] |= 0x80;                 // QR = response
        buf[3] = 0x00;                  // RA=0, RCODE=0
        buf[6] = 0; buf[7] = 1;         // ANCOUNT = 1
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;

        int qend = 12;
        while (qend < n && buf[qend] != 0) { qend += buf[qend] + 1; }
        qend += 1 + 4;                  // null label + QTYPE + QCLASS
        if (qend + 16 > (int)sizeof(buf) || qend > n) { continue; }

        uint8_t *a = buf + qend;
        a[0] = 0xC0; a[1] = 0x0C;       // name pointer to question
        a[2] = 0x00; a[3] = 0x01;       // TYPE A
        a[4] = 0x00; a[5] = 0x01;       // CLASS IN
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60;   // TTL 60
        a[10] = 0x00; a[11] = 0x04;     // RDLENGTH 4
        a[12] = 192; a[13] = 168; a[14] = 4; a[15] = 1;

        sendto(s_sock, buf, qend + 16, 0, (struct sockaddr *)&from, fl);
    }
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (s_task) { return; }
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, &s_task);
}

void captive_dns_stop(void)
{
    if (s_sock >= 0) { shutdown(s_sock, 0); close(s_sock); s_sock = -1; }
    s_task = NULL;   // task self-deletes on socket error/close
}
