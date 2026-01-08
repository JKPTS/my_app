// ===== FILE: main/expfs.h =====
#pragma once
#include <stdint.h>
#include "esp_err.h"

void expfs_start(void);

// last ADC raw (0..4095 typical)
uint16_t expfs_get_last_raw(int port);

// save calibration using current raw
esp_err_t expfs_cal_save(int port, int which_min0_max1);
