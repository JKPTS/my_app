// ===== FILE: main/display_uart.h =====
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void display_uart_init(void);

// request refresh of current bank/switch names (non-blocking)
void display_uart_request_refresh(void);

#ifdef __cplusplus
}
#endif
