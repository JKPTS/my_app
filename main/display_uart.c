// ===== FILE: main/display_uart.c =====
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "driver/uart.h"

#include "config_store.h"
#include "display_uart.h"

static const char *TAG = "DISP_UART";

// use UART0 pins per user request (ESP32-S3)
#define DISP_UART_NUM      UART_NUM_0
#define DISP_UART_BAUD     115200
#define DISP_UART_TX_PIN   43  // U0TXD
#define DISP_UART_RX_PIN   44  // U0RXD

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_pending = false;

static void sanitize_commas(char *s)
{
    if (!s) return;
    for (; *s; s++) {
        if (*s == ',') *s = ' ';
        if (*s == '\n' || *s == '\r') *s = ' ';
    }
}

static bool wait_ack_ms(int timeout_ms)
{
    uint8_t buf[128];
    int got_total = 0;
    char acc[256];
    acc[0] = 0;

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int n = uart_read_bytes(DISP_UART_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(20));
        elapsed += 20;
        if (n > 0) {
            if (got_total + n >= (int)sizeof(acc) - 1) {
                // reset buffer if overflow
                got_total = 0;
                acc[0] = 0;
            }
            memcpy(acc + got_total, buf, n);
            got_total += n;
            acc[got_total] = 0;

            if (strstr(acc, "@A,SAVED")) return true;
        }
    }
    return false;
}

static void build_msg(char *out, size_t out_len)
{
    if (!out || out_len < 8) return;
    out[0] = 0;

    const foot_config_t *cfg = config_store_get();
    if (!cfg) {
        snprintf(out, out_len, "@U,0,NA,NA,NA,NA,NA,NA,NA,NA\n");
        return;
    }

    int bank = (int)config_store_get_current_bank();
    if (bank < 0) bank = 0;
    if (bank >= (int)cfg->bank_count) bank = 0;

    char bn[NAME_LEN];
    strncpy(bn, cfg->bank_name[bank], sizeof(bn));
    bn[NAME_LEN - 1] = 0;
    sanitize_commas(bn);

    // message: @U,<bank>,<bankname>,<sw1>..,<sw8>\n
    int pos = snprintf(out, out_len, "@U,%d,%s", bank, bn);
    for (int k = 0; k < NUM_BTNS; k++) {
        char sn[NAME_LEN];
        strncpy(sn, cfg->switch_name[bank][k], sizeof(sn));
        sn[NAME_LEN - 1] = 0;
        sanitize_commas(sn);
        pos += snprintf(out + pos, (pos < (int)out_len) ? out_len - (size_t)pos : 0, ",%s", sn);
    }
    snprintf(out + pos, (pos < (int)out_len) ? out_len - (size_t)pos : 0, "\n");
}

static void disp_task(void *arg)
{
    (void)arg;

    char msg[256];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // debounce & coalesce
        vTaskDelay(pdMS_TO_TICKS(80));
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pending = false;
        if (s_lock) xSemaphoreGive(s_lock);

        // flush RX to reduce stale noise
        uart_flush_input(DISP_UART_NUM);

        build_msg(msg, sizeof(msg));
        uart_write_bytes(DISP_UART_NUM, msg, (int)strlen(msg));

        // wait ACK (best-effort). Do not block other tasks.
        if (!wait_ack_ms(600)) {
            ESP_LOGW(TAG, "no ACK (err=ESP_ERR_TIMEOUT)");
        }
    }
}

void display_uart_init(void)
{
    if (s_task) return;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = DISP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(DISP_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DISP_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DISP_UART_NUM, DISP_UART_TX_PIN, DISP_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreatePinnedToCore(disp_task, "disp_uart", 3072, NULL, 6, &s_task, 0);

    // send once on boot
    display_uart_request_refresh();
    ESP_LOGI(TAG, "display uart ready (U0 tx=%d rx=%d)", DISP_UART_TX_PIN, DISP_UART_RX_PIN);
}

void display_uart_request_refresh(void)
{
    if (!s_task) return;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_pending) {
        s_pending = true;
        xTaskNotifyGive(s_task);
    } else {
        // already pending; just notify again to coalesce latest
        xTaskNotifyGive(s_task);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}
