// ===== FILE: main/uart_midi_out.h =====
#pragma once
#include <stdint.h>
#include "esp_err.h"

// init UART MIDI OUT (31250 8N1)
void uart_midi_out_init(void);

// quick ready check
int uart_midi_out_ready_fast(void);

// sending helpers
esp_err_t uart_midi_send_cc(uint8_t ch_1_16, uint8_t cc, uint8_t val);
esp_err_t uart_midi_send_pc(uint8_t ch_1_16, uint8_t pc);
esp_err_t uart_midi_send_note_on(uint8_t ch_1_16, uint8_t note, uint8_t vel);
esp_err_t uart_midi_send_note_off(uint8_t ch_1_16, uint8_t note, uint8_t vel);
esp_err_t uart_midi_send_rt(uint8_t rt_byte);
