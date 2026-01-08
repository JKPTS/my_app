// ===== FILE: main/midi_actions.h =====
#pragma once
#include "config_store.h"

// event:
#define MIDI_EVT_TRIGGER 0  // one-shot (short/long/immediate)
#define MIDI_EVT_DOWN    1  // press-down
#define MIDI_EVT_UP      2  // release

void midi_actions_run(const action_t *actions, int n, cc_behavior_t cc_behavior, int event);
