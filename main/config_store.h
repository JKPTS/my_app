// ===== FILE: main/config_store.h =====
#pragma once
#include "esp_err.h"
#include <stdint.h>

#define MAX_BANKS   100
#define NUM_BTNS    8
#define MAX_ACTIONS 20

#define NAME_LEN    16   // รวม '\0'

typedef enum {
    ACT_NONE = 0,
    ACT_CC,
    ACT_PC,

    // legacy types (จะถูก sanitize ให้เป็น NONE ตอน boot)
    ACT_NOTE,
    ACT_DELAY,
    ACT_BANK_PC,
} action_type_t;

typedef enum {
    BTN_SHORT           = 0,
    BTN_SHORT_LONG      = 1,
    BTN_TOGGLE          = 2,

    // ✅ group (ใช้เฉพาะ footswitch 8 ปุ่มหลัก)
    BTN_SHORT_GROUP_LED = 3,
} btn_press_mode_t;

typedef enum {
    CC_NORMAL    = 0,
    CC_TOGGLE    = 1,
    CC_MOMENTARY = 2,
} cc_behavior_t;

typedef struct {
    action_type_t type;
    uint8_t ch;      // 1..16
    uint8_t a;
    uint8_t b;
    uint8_t c;
} action_t;

typedef struct {
    btn_press_mode_t press_mode;
    cc_behavior_t cc_behavior;
    action_t short_actions[MAX_ACTIONS];
    action_t long_actions[MAX_ACTIONS];
} btn_map_t;

typedef struct {
    // layout
    uint8_t bank_count;                       // 1..MAX_BANKS
    char    bank_name[MAX_BANKS][NAME_LEN];

    // ✅ switch name ต่อ bank (8 ปุ่ม)
    char    switch_name[MAX_BANKS][NUM_BTNS][NAME_LEN];

    // mapping
    btn_map_t map[MAX_BANKS][NUM_BTNS];
} foot_config_t;

// -------------------- exp/fs --------------------
#define EXPFS_PORT_COUNT 2

typedef enum {
    EXPFS_KIND_EXP       = 0,
    EXPFS_KIND_SINGLE_SW = 1,
    EXPFS_KIND_DUAL_SW   = 2,
} expfs_kind_t;

typedef struct {
    btn_press_mode_t press_mode;   // 0..2 only (no group led)
    cc_behavior_t    cc_behavior;
    action_t short_actions[MAX_ACTIONS];
    action_t long_actions[MAX_ACTIONS];
} expfs_btncfg_t;

typedef struct {
    expfs_kind_t kind;

    // exp: one command only
    // - if CC: a=cc#, b=val1, c=val2
    // - if PC: a=val1, b=val2, c=0
    action_t exp_action;

    // calibration raw ADC points
    // min = pedal down (toe), max = pedal up (heel)
    uint16_t cal_min;
    uint16_t cal_max;

    // switch configs
    expfs_btncfg_t tip;   // used for single/dual
    expfs_btncfg_t ring;  // used only for dual
} expfs_port_cfg_t;

// ---- init/load/save ----
void config_store_init(void);
const foot_config_t *config_store_get(void);

// ---- layout helpers ----
int  config_store_bank_count(void);
const char *config_store_bank_name(int bank);

// ---- layout json ----
esp_err_t config_store_get_layout_json(char *out, int out_len);
esp_err_t config_store_set_layout_json(const char *json);

// ---- bank (switch names) json ----
esp_err_t config_store_get_bank_json(int bank, char *out, int out_len);
esp_err_t config_store_set_bank_json(int bank, const char *json);

// ---- per-button json ----
esp_err_t config_store_get_btn_json(int bank, int btn, char *out, int out_len);
esp_err_t config_store_set_btn_json(int bank, int btn, const char *json);

// ---- led brightness (global, 0..100) ----
uint8_t  config_store_get_led_brightness(void);
esp_err_t config_store_set_led_brightness(uint8_t percent);

// ---- a+b led select (per bank/button): 0=A, 1=B ----
uint8_t  config_store_get_ab_led_sel(int bank, int btn);
esp_err_t config_store_set_ab_led_sel(int bank, int btn, uint8_t sel);

// ---- current bank persistence ----
uint8_t  config_store_get_current_bank(void);
esp_err_t config_store_set_current_bank(uint8_t bank);

// ---- exp/fs API ----
const expfs_port_cfg_t *config_store_get_expfs_cfg(int port);
esp_err_t config_store_get_expfs_json(int port, char *out, int out_len);
esp_err_t config_store_set_expfs_json(int port, const char *json);

// calibration save helper (persist)
esp_err_t config_store_set_expfs_cal(int port, int which_min0_max1, uint16_t raw);
