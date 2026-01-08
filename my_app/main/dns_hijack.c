#include <string.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "DNS";

static void dns_task(void *arg)
{
    (void)arg;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];

    while (1) {
        struct sockaddr_in from = {0};
        socklen_t flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
        if (n < 12) continue;

        // Minimal DNS response:
        uint8_t out[512];

        // เราจะ append อีก 16 bytes ตอนท้าย (A record answer)
        const int append_len = 16;

        // ✅ กัน overflow: ถ้าแพ็กเก็ตใหญ่เกินพื้นที่ที่เหลือ -> ทิ้ง
        if (n > (int)sizeof(out) - append_len) {
            // (optional) log เป็น debug ได้
            // ESP_LOGW(TAG, "dns pkt too large: n=%d", n);
            continue;
        }

        memcpy(out, buf, n);

        // Header: set QR=1 response, RCODE=0, ANCOUNT=1
        out[2] = 0x81;
        out[3] = 0x80;
        out[6] = 0x00; out[7] = 0x01;

        // Answer:
        // NAME pointer 0xC00C, TYPE=A, CLASS=IN, TTL=60, RDLEN=4, RDATA=192.168.4.1
        int p = n;
        out[p++] = 0xC0; out[p++] = 0x0C;
        out[p++] = 0x00; out[p++] = 0x01;
        out[p++] = 0x00; out[p++] = 0x01;
        out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x3C;
        out[p++] = 0x00; out[p++] = 0x04;
        out[p++] = 192; out[p++] = 168; out[p++] = 4; out[p++] = 1;

        sendto(s, out, p, 0, (struct sockaddr*)&from, flen);
    }
}

void dns_hijack_start(void)
{
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS hijack started");
}
