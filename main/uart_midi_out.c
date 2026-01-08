// ===== FILE: main/uart_midi_out.c =====
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "uart_midi_out.h"

static const char *TAG = "UART_MIDI";

// ---- config ----
// ใช้ UART1 เพื่อเลี่ยงชนกับ USB-serial (U0TXD/U0RXD)
#define UART_MIDI_PORT      UART_NUM_1
#define UART_MIDI_BAUD      31250

// แนะนำ: GPIO17 = U1TXD
#define UART_MIDI_TX_GPIO   17
#define UART_MIDI_RX_GPIO   (-1)   // not used
#define UART_MIDI_RTS_GPIO  (-1)
#define UART_MIDI_CTS_GPIO  (-1)

static int s_inited = 0;

static inline uint8_t clamp7(int v)  { if (v < 0) return 0; if (v > 127) return 127; return (uint8_t)v; }
static inline uint8_t clampCh(int v) { if (v < 1) return 1; if (v > 16) return 16; return (uint8_t)v; }

void uart_midi_out_init(void)
{
    if (s_inited) {
        ESP_LOGW(TAG, "already inited");
        return;
    }

    uart_config_t cfg = {
        .baud_rate = UART_MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t e = uart_param_config(UART_MIDI_PORT, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(e));
        return;
    }

    e = uart_set_pin(UART_MIDI_PORT,
                     UART_MIDI_TX_GPIO,
                     UART_MIDI_RX_GPIO,
                     UART_MIDI_RTS_GPIO,
                     UART_MIDI_CTS_GPIO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(e));
        return;
    }

    // ✅ แม้จะ TX อย่างเดียว ก็ใส่ RX buffer > 0 กัน ESP_ERR_INVALID_ARG
    // TX buffer = 0 => uart_write_bytes จะส่งแบบ blocking ได้
    e = uart_driver_install(UART_MIDI_PORT, 256, 0, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(e));
        return;
    }

    s_inited = 1;
    ESP_LOGI(TAG, "UART MIDI OUT ready: port=%d tx=GPIO%d baud=%d",
             (int)UART_MIDI_PORT, UART_MIDI_TX_GPIO, UART_MIDI_BAUD);
}

int uart_midi_out_ready_fast(void)
{
    return s_inited;
}

static esp_err_t uart_midi_send_bytes(const uint8_t *b, int n)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!b || n <= 0) return ESP_ERR_INVALID_ARG;

    int w = uart_write_bytes(UART_MIDI_PORT, (const char *)b, n);
    if (w != n) return ESP_FAIL;

    // ไม่จำเป็นต้องรอ TX done ก็ได้ แต่ใส่ไว้ให้ชัวร์
    (void)uart_wait_tx_done(UART_MIDI_PORT, pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t uart_midi_send_cc(uint8_t ch_1_16, uint8_t cc, uint8_t val)
{
    ch_1_16 = clampCh(ch_1_16);
    uint8_t pkt[3] = {
        (uint8_t)(0xB0 | ((ch_1_16 - 1) & 0x0F)),
        (uint8_t)(clamp7(cc) & 0x7F),
        (uint8_t)(clamp7(val) & 0x7F),
    };
    return uart_midi_send_bytes(pkt, 3);
}

esp_err_t uart_midi_send_pc(uint8_t ch_1_16, uint8_t pc)
{
    ch_1_16 = clampCh(ch_1_16);
    uint8_t pkt[2] = {
        (uint8_t)(0xC0 | ((ch_1_16 - 1) & 0x0F)),
        (uint8_t)(clamp7(pc) & 0x7F),
    };
    return uart_midi_send_bytes(pkt, 2);
}

esp_err_t uart_midi_send_note_on(uint8_t ch_1_16, uint8_t note, uint8_t vel)
{
    ch_1_16 = clampCh(ch_1_16);
    uint8_t pkt[3] = {
        (uint8_t)(0x90 | ((ch_1_16 - 1) & 0x0F)),
        (uint8_t)(clamp7(note) & 0x7F),
        (uint8_t)(clamp7(vel) & 0x7F),
    };
    return uart_midi_send_bytes(pkt, 3);
}

esp_err_t uart_midi_send_note_off(uint8_t ch_1_16, uint8_t note, uint8_t vel)
{
    ch_1_16 = clampCh(ch_1_16);
    uint8_t pkt[3] = {
        (uint8_t)(0x80 | ((ch_1_16 - 1) & 0x0F)),
        (uint8_t)(clamp7(note) & 0x7F),
        (uint8_t)(clamp7(vel) & 0x7F),
    };
    return uart_midi_send_bytes(pkt, 3);
}

esp_err_t uart_midi_send_rt(uint8_t rt_byte)
{
    uint8_t b = rt_byte;
    return uart_midi_send_bytes(&b, 1);
}
