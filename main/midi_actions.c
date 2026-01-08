// ===== FILE: main/midi_actions.c =====
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "midi_actions.h"
#include "usb_midi_host.h"
#include "uart_midi_out.h"

static const char *TAG = "MIDI_ACT";

// ✅ moved toggle table to heap (PSRAM first) to save internal DRAM (.bss)
static uint8_t *s_toggle = NULL; // size = 16*128

static inline uint8_t clamp7(int v)  { if (v < 0) return 0; if (v > 127) return 127; return (uint8_t)v; }
static inline uint8_t clampCh(int v) { if (v < 1) return 1; if (v > 16) return 16; return (uint8_t)v; }

static inline size_t tog_idx(uint8_t ch /*1..16*/, uint8_t cc /*0..127*/)
{
    return (size_t)((ch - 1u) * 128u + cc);
}

static void toggle_init_once(void)
{
    if (s_toggle) return;

    const size_t bytes = 16u * 128u;

    s_toggle = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_toggle) s_toggle = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);

    if (s_toggle) {
        memset(s_toggle, 0, bytes);
        ESP_LOGI(TAG, "toggle table allocated (%u bytes)", (unsigned)bytes);
    } else {
        ESP_LOGE(TAG, "no heap for toggle table (%u bytes) -> CC_TOGGLE will degrade", (unsigned)bytes);
    }
}

static inline void send_cc_all(uint8_t ch, uint8_t cc, uint8_t val)
{
    if (usb_midi_ready_fast()) (void)usb_midi_send_cc(ch, cc, val);
    if (uart_midi_out_ready_fast()) (void)uart_midi_send_cc(ch, cc, val);
}

static inline void send_pc_all(uint8_t ch, uint8_t pc)
{
    if (usb_midi_ready_fast()) (void)usb_midi_send_pc(ch, pc);
    if (uart_midi_out_ready_fast()) (void)uart_midi_send_pc(ch, pc);
}

void midi_actions_run(const action_t *actions, int n, cc_behavior_t cc_behavior, int event)
{
    const int usb_ok  = usb_midi_ready_fast();
    const int uart_ok = uart_midi_out_ready_fast();

    // ✅ ถ้าไม่มีทางส่งออกเลย -> drop
    if (!usb_ok && !uart_ok) return;

    // only allocate when we might need it
    if (cc_behavior == CC_TOGGLE) {
        toggle_init_once();
    }

    for (int i = 0; i < n; i++) {
        const action_t *a = &actions[i];
        if (a->type == ACT_NONE) continue;

        uint8_t ch = clampCh(a->ch);

        if (a->type == ACT_CC) {
            uint8_t cc = clamp7(a->a);
            uint8_t valA = clamp7(a->b);
            uint8_t valB = 0;

            if (cc_behavior == CC_NORMAL) {
                if (event != MIDI_EVT_TRIGGER) continue;
                send_cc_all(ch, cc, valA);

            } else if (cc_behavior == CC_TOGGLE) {
                if (event != MIDI_EVT_TRIGGER) continue;

                // if no toggle table available -> behave like NORMAL (no crash)
                if (!s_toggle) {
                    send_cc_all(ch, cc, valA);
                    continue;
                }

                uint8_t *st = &s_toggle[tog_idx(ch, cc)];
                *st = (uint8_t)!(*st);

                uint8_t outv = (*st) ? valA : valB;
                send_cc_all(ch, cc, outv);

            } else if (cc_behavior == CC_MOMENTARY) {
                if (event == MIDI_EVT_DOWN) {
                    send_cc_all(ch, cc, valA);
                } else if (event == MIDI_EVT_UP) {
                    send_cc_all(ch, cc, valB);
                }
            }
            continue;
        }

        if (event != MIDI_EVT_TRIGGER) continue;

        if (a->type == ACT_PC) {
            uint8_t pc = clamp7(a->a);
            send_pc_all(ch, pc);
            continue;
        }
    }
}
