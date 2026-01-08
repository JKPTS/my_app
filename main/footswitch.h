// ===== FILE: main/footswitch.h =====
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t bank; // 0..bankCount-1
} footswitch_state_t;

void footswitch_start(void);

footswitch_state_t footswitch_get_state(void);
void footswitch_set_bank(int bank);
