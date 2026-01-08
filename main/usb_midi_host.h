// ===== FILE: main/usb_midi_host.h =====
#pragma once
#include <stdint.h>
#include "esp_err.h"

void usb_midi_host_init(void);

// ready check
int usb_midi_ready_fast(void);

// sending
esp_err_t usb_midi_send_cc(uint8_t ch_1_16, uint8_t cc, uint8_t val);
esp_err_t usb_midi_send_pc(uint8_t ch_1_16, uint8_t pc);
esp_err_t usb_midi_send_note_on(uint8_t ch_1_16, uint8_t note, uint8_t vel);
esp_err_t usb_midi_send_note_off(uint8_t ch_1_16, uint8_t note, uint8_t vel);

// ✅ realtime (midi clock etc.)
esp_err_t usb_midi_send_rt(uint8_t rt_byte);
