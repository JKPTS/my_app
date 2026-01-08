// ===== FILE: main/footswitch.c =====
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "footswitch.h"
#include "config_store.h"
#include "midi_actions.h"

static const char *TAG = "FOOTSW";

// -------------------- switches --------------------
static const gpio_num_t sw_pins[8] = {
    (gpio_num_t)42, (gpio_num_t)41, (gpio_num_t)40, (gpio_num_t)39,
    (gpio_num_t)4,  (gpio_num_t)5,  (gpio_num_t)6,  (gpio_num_t)7
};

// -------------------- leds --------------------
static const gpio_num_t led_pins[8] = {
    (gpio_num_t)8,  (gpio_num_t)3,  (gpio_num_t)9,  (gpio_num_t)10,
    (gpio_num_t)11, (gpio_num_t)12, (gpio_num_t)13, (gpio_num_t)14
};

// 0 = active-high (1=ติด), 1 = active-low (0=ติด)
#define LED_ACTIVE_LOW 0

static footswitch_state_t s_state = {0};

static inline int wrapi(int v, int max)
{
    if (max <= 0) return 0;
    int r = v % max;
    if (r < 0) r += max;
    return r;
}

static inline int pressed(int idx) { return gpio_get_level(sw_pins[idx]) == 0; } // pull-up: pressed=0

// -------------------- led (PWM) helpers --------------------
static const ledc_mode_t  LEDC_MODE  = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;

static const ledc_channel_t LEDC_CH[8] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
    LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6, LEDC_CHANNEL_7
};

static uint8_t  s_led_on[8];
static uint8_t  s_brightness = 100;     // 0..100
static uint32_t s_duty_max = 8191;      // 13-bit default

static void ledc_apply_one(int idx)
{
    if (idx < 0 || idx >= 8) return;

    uint32_t duty = 0;
    uint32_t scaled = (s_duty_max * (uint32_t)s_brightness) / 100u;

#if LED_ACTIVE_LOW
    if (s_led_on[idx]) duty = (s_duty_max >= scaled) ? (s_duty_max - scaled) : 0;
    else duty = s_duty_max;
#else
    if (s_led_on[idx]) duty = scaled;
    else duty = 0;
#endif

    ledc_set_duty(LEDC_MODE, LEDC_CH[idx], duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH[idx]);
}

static void led_apply_all(void)
{
    for (int i = 0; i < 8; i++) ledc_apply_one(i);
}

static void led_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (s_brightness == percent) return;
    s_brightness = percent;
    led_apply_all();
}

static inline void led_write_raw(int idx, int on)
{
    if (idx < 0 || idx >= 8) return;
    uint8_t v = on ? 1u : 0u;
    if (s_led_on[idx] == v) return;
    s_led_on[idx] = v;
    ledc_apply_one(idx);
}

static inline void led_on(int idx)  { led_write_raw(idx, 1); }
static inline void led_off(int idx) { led_write_raw(idx, 0); }

// -------------------- run helpers --------------------
static void run_actions_trigger_list(const action_t *list, cc_behavior_t cc_beh)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, MIDI_EVT_TRIGGER);
}

static void run_actions_down_up_list(const action_t *list, cc_behavior_t cc_beh, int event)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, event);
}

footswitch_state_t footswitch_get_state(void) { return s_state; }

void footswitch_set_bank(int bank)
{
    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);

    s_state.bank = (uint8_t)bank;

    // ✅ persist current bank (so reboot stays here)
    (void)config_store_set_current_bank((uint8_t)bank);
}

// -------------------- combo / nav lock --------------------
// combo:
// 5&6 -> bank--
// 7&8 -> bank++
static uint8_t s_combo_mask = 0;

// ✅ NEW: lock ทุกปุ่มระหว่างยังค้างคอมโบอยู่
static uint8_t s_nav_lock = 0;
static uint8_t s_nav_hold_mask = 0;     // ปุ่มที่ต้องปล่อยครบถึงปลดล็อก
static uint8_t s_nav_consumed_mask = 0; // ปุ่มที่ถูกใช้เป็นคอมโบแล้ว ห้ามยิง action ใด ๆ
static uint8_t s_nav_pending_mask = 0;  // ปุ่ม 5-8 ที่ถูกกดเดี่ยว ๆ (defer ยิงตอนปล่อย)

// helper: เช็คว่ามีปุ่มใน mask ยังค้างอยู่ไหม
static inline int mask_any_pressed(uint8_t mask)
{
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) {
            if (pressed(i)) return 1;
        }
    }
    return 0;
}

static void apply_combo_logic(void)
{
    int b5 = pressed(4);
    int b6 = pressed(5);
    int b7 = pressed(6);
    int b8 = pressed(7);

    // ถ้าล็อกอยู่: รอจนปล่อยคอมโบครบ 2 ปุ่ม
    if (s_nav_lock) {
        if (!mask_any_pressed(s_nav_hold_mask)) {
            s_nav_lock = 0;
            s_nav_hold_mask = 0;
            s_nav_consumed_mask = 0;
            s_combo_mask = 0;
        }
        return;
    }

    // detect combo ทันที (ไม่หน่วง)
    if (b5 && b6) {
        footswitch_set_bank((int)s_state.bank - 1);

        s_combo_mask = (1u << 4) | (1u << 5);
        s_nav_lock = 1;
        s_nav_hold_mask = s_combo_mask;
        s_nav_consumed_mask = s_combo_mask;

        // ✅ ปุ่มคู่นี้ถูกใช้เป็นคอมโบแล้ว => ยกเลิก pending (กันไม่ให้ไปยิงตอนปล่อย)
        s_nav_pending_mask &= (uint8_t)~s_combo_mask;
        return;
    }

    if (b7 && b8) {
        footswitch_set_bank((int)s_state.bank + 1);

        s_combo_mask = (1u << 6) | (1u << 7);
        s_nav_lock = 1;
        s_nav_hold_mask = s_combo_mask;
        s_nav_consumed_mask = s_combo_mask;

        s_nav_pending_mask &= (uint8_t)~s_combo_mask;
        return;
    }

    // ไม่มีคอมโบ: ไม่ต้องแตะ mask (pending จะตัดสินใจตอนปล่อย)
}

// -------------------- dynamic state (heap/PSRAM) --------------------
typedef struct {
    uint8_t *ab_state;      // [MAX_BANKS][NUM_BTNS] 0=A,1=B
    uint8_t *group_sel;     // [MAX_BANKS] selected index 0..7 or 0xFF
    uint8_t  pressed_sel[NUM_BTNS]; // 0=A,1=B (per button for momentary toggle)
    uint8_t  inited;
} foot_dyn_t;

static foot_dyn_t s_dyn;

static inline size_t idx_ab(int bank, int btn)
{
    return (size_t)(bank * NUM_BTNS + btn);
}

static void dyn_state_init_once(void)
{
    if (s_dyn.inited) return;

    const size_t ab_bytes = (size_t)MAX_BANKS * (size_t)NUM_BTNS;
    const size_t gp_bytes = (size_t)MAX_BANKS;

    s_dyn.ab_state = (uint8_t *)heap_caps_malloc(ab_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dyn.ab_state) s_dyn.ab_state = (uint8_t *)heap_caps_malloc(ab_bytes, MALLOC_CAP_8BIT);

    s_dyn.group_sel = (uint8_t *)heap_caps_malloc(gp_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dyn.group_sel) s_dyn.group_sel = (uint8_t *)heap_caps_malloc(gp_bytes, MALLOC_CAP_8BIT);

    memset(s_dyn.pressed_sel, 0, sizeof(s_dyn.pressed_sel));

    if (s_dyn.ab_state) {
        memset(s_dyn.ab_state, 0, ab_bytes);
    } else {
        ESP_LOGE(TAG, "no heap for ab_state (%u bytes) -> toggle state won't persist", (unsigned)ab_bytes);
    }

    if (s_dyn.group_sel) {
        memset(s_dyn.group_sel, 0xFF, gp_bytes);
    } else {
        ESP_LOGE(TAG, "no heap for group_sel (%u bytes) -> group led won't persist", (unsigned)gp_bytes);
    }

    s_dyn.inited = 1;
}

static inline uint8_t dyn_get_ab(int bank, int btn)
{
    if (!s_dyn.ab_state) return 0;
    return s_dyn.ab_state[idx_ab(bank, btn)] ? 1u : 0u;
}

static inline void dyn_set_ab(int bank, int btn, uint8_t v)
{
    if (!s_dyn.ab_state) return;
    s_dyn.ab_state[idx_ab(bank, btn)] = (v ? 1u : 0u);
}

static inline uint8_t dyn_get_group(int bank)
{
    if (!s_dyn.group_sel) return 0xFF;
    return s_dyn.group_sel[bank];
}

static inline void dyn_set_group(int bank, uint8_t v)
{
    if (!s_dyn.group_sel) return;
    s_dyn.group_sel[bank] = v;
}

static inline int is_nav_candidate_btn(int i)
{
    // ปุ่ม 5-8 (index 4..7) เป็นปุ่มที่สามารถเข้า combo bank ได้
    return (i >= 4 && i <= 7);
}

static void foot_task(void *arg)
{
    (void)arg;

    dyn_state_init_once();

    // inputs
    gpio_config_t io = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < 8; i++) io.pin_bit_mask |= (1ULL << sw_pins[i]);
    gpio_config(&io);

    // ledc timer + channels
    ledc_timer_config_t tc = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    s_duty_max = (1u << 13) - 1u;

    for (int i = 0; i < 8; i++) {
        ledc_channel_config_t cc = {
            .gpio_num   = led_pins[i],
            .speed_mode = LEDC_MODE,
            .channel    = LEDC_CH[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        ledc_channel_config(&cc);
    }

    // init cache
    memset(s_led_on, 0, sizeof(s_led_on));
    s_brightness = config_store_get_led_brightness();
    if (s_brightness > 100) s_brightness = 100;

    // default: turn all ON (guide)
    for (int i = 0; i < 8; i++) s_led_on[i] = 1;
    led_apply_all();

    uint8_t last[8];
    for (int i = 0; i < 8; i++) last[i] = 1;

    const int LONG_MS = 400;

    int hold_ms[8] = {0};
    uint8_t long_fired[8] = {0};

    uint8_t last_bri = s_brightness;

    while (1) {
        apply_combo_logic();

        // live brightness update
        uint8_t bri = config_store_get_led_brightness();
        if (bri > 100) bri = 100;
        if (bri != last_bri) {
            last_bri = bri;
            led_set_brightness(bri);
        }

        const foot_config_t *cfg = config_store_get();
        int bank = (int)s_state.bank;

        if (!cfg) {
            for (int i = 0; i < 8; i++) {
                int now = gpio_get_level(sw_pins[i]);
                if (now == 0) led_off(i);
                else led_on(i);
                last[i] = (uint8_t)now;
                hold_ms[i] = 0;
                long_fired[i] = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (int i = 0; i < 8; i++) {
            int now = gpio_get_level(sw_pins[i]); // 0 pressed, 1 released
            const btn_map_t *m = &cfg->map[bank][i];

            // ✅ NEW: ระหว่าง nav lock ห้ามปุ่มอื่นยิงค่าใด ๆ
            // ต้องกดใหม่หลังปลดล็อกเท่านั้น
            if (s_nav_lock && !(s_nav_hold_mask & (1u << i))) {
                last[i] = (uint8_t)now;
                hold_ms[i] = 0;
                long_fired[i] = 0;
                continue;
            }

            // ✅ ปุ่มที่ถูกใช้เป็นคอมโบแล้ว: ห้ามยิง action ใด ๆ
            if (s_nav_consumed_mask & (1u << i)) {
                last[i] = (uint8_t)now;
                hold_ms[i] = 0;
                long_fired[i] = 0;
                continue;
            }

            // (กันปุ่มคอมโบไว้เหมือนเดิม)
            if (s_combo_mask & (1u << i)) {
                last[i] = (uint8_t)now;
                hold_ms[i] = 0;
                long_fired[i] = 0;
                continue;
            }

            const action_t *listA = m->short_actions;
            const action_t *listB = m->long_actions;

            // ✅ NEW: ปุ่ม 5-8 ทำเป็น "defer" เพื่อกันยิง CC ก่อนจะเข้าคอมโบ
            if (is_nav_candidate_btn(i)) {
                // edge: down -> mark pending (ยังไม่ยิงอะไร)
                if (last[i] == 1 && now == 0) {
                    s_nav_pending_mask |= (1u << i);
                    hold_ms[i] = 0;
                    long_fired[i] = 0;
                    last[i] = (uint8_t)now;
                    continue;
                }

                // hold (เก็บเวลาไว้ตัดสิน short/long ตอนปล่อย)
                if (now == 0) {
                    if (s_nav_pending_mask & (1u << i)) {
                        hold_ms[i] += 10;
                        last[i] = (uint8_t)now;
                        continue;
                    }
                }

                // edge: up -> ถ้า pending และไม่ได้ถูก consume เป็นคอมโบ -> ยิงตอนปล่อย
                if (last[i] == 0 && now == 1) {
                    if (s_nav_pending_mask & (1u << i)) {
                        // clear pending ก่อน
                        s_nav_pending_mask &= (uint8_t)~(1u << i);

                        // ทำงาน "ตอนปล่อย" เท่านั้น (กันกรณีคอมโบ)
                        if (m->press_mode == BTN_SHORT_GROUP_LED) {
                            run_actions_trigger_list(listA, m->cc_behavior);
                            dyn_set_group(bank, (uint8_t)i);
                        } else if (m->press_mode == BTN_TOGGLE) {
                            uint8_t st = dyn_get_ab(bank, i) ? 1 : 0;
                            run_actions_trigger_list(st ? listB : listA, m->cc_behavior);
                            dyn_set_ab(bank, i, (uint8_t)!st);
                        } else if (m->press_mode == BTN_SHORT_LONG) {
                            if (hold_ms[i] >= LONG_MS) run_actions_trigger_list(listB, m->cc_behavior);
                            else run_actions_trigger_list(listA, m->cc_behavior);
                        } else {
                            // BTN_SHORT หรืออื่น ๆ -> short
                            run_actions_trigger_list(listA, m->cc_behavior);
                        }

                        hold_ms[i] = 0;
                        long_fired[i] = 0;
                        last[i] = (uint8_t)now;
                        continue;
                    }
                }

                last[i] = (uint8_t)now;
                continue;
            }

            // -------------------- NORMAL buttons (1-4) --------------------
            // edge: down
            if (last[i] == 1 && now == 0) {
                hold_ms[i] = 0;
                long_fired[i] = 0;

                // momentary: DOWN
                if (m->cc_behavior == CC_MOMENTARY) {
                    if (m->press_mode == BTN_TOGGLE) {
                        uint8_t st = dyn_get_ab(bank, i) ? 1 : 0;
                        s_dyn.pressed_sel[i] = st;
                        run_actions_down_up_list(st ? listB : listA, m->cc_behavior, MIDI_EVT_DOWN);
                    } else {
                        run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_DOWN);
                    }
                }

                // group: trigger + select
                if (m->press_mode == BTN_SHORT_GROUP_LED) {
                    run_actions_trigger_list(listA, m->cc_behavior);
                    dyn_set_group(bank, (uint8_t)i);
                }

                // toggle: trigger + flip A/B
                if (m->press_mode == BTN_TOGGLE) {
                    uint8_t st = dyn_get_ab(bank, i) ? 1 : 0;
                    run_actions_trigger_list(st ? listB : listA, m->cc_behavior);
                    dyn_set_ab(bank, i, (uint8_t)!st);
                }
            }

            // hold
            if (now == 0) {
                hold_ms[i] += 10;

                if (m->press_mode == BTN_SHORT_LONG && !long_fired[i] && hold_ms[i] >= LONG_MS) {
                    run_actions_trigger_list(listB, m->cc_behavior);
                    long_fired[i] = 1;
                }
            }

            // edge: up
            if (last[i] == 0 && now == 1) {
                if (m->cc_behavior == CC_MOMENTARY) {
                    if (m->press_mode == BTN_TOGGLE) {
                        uint8_t sel = s_dyn.pressed_sel[i] ? 1 : 0;
                        run_actions_down_up_list(sel ? listB : listA, m->cc_behavior, MIDI_EVT_UP);
                    } else {
                        run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_UP);
                    }
                }

                // short: fire on release
                if (m->press_mode == BTN_SHORT) {
                    run_actions_trigger_list(listA, m->cc_behavior);
                }

                if (m->press_mode == BTN_SHORT_LONG) {
                    if (!long_fired[i] && hold_ms[i] < LONG_MS) {
                        run_actions_trigger_list(listA, m->cc_behavior);
                    }
                }

                hold_ms[i] = 0;
                long_fired[i] = 0;
            }

            last[i] = (uint8_t)now;
        }

        // -------------------- LED render pass --------------------
        for (int i = 0; i < 8; i++) {
            const btn_map_t *m = &cfg->map[bank][i];
            int is_down = (gpio_get_level(sw_pins[i]) == 0);

            // group mode
            if (m->press_mode == BTN_SHORT_GROUP_LED) {
                uint8_t sel = dyn_get_group(bank);
                int on = (sel == (uint8_t)i) ? 1 : 0;
                if (is_down) on = 0;
                if (on) led_on(i); else led_off(i);
                continue;
            }

            // toggle: a+b led select (0=A,1=B)
            if (m->press_mode == BTN_TOGGLE) {
                uint8_t ledsel = config_store_get_ab_led_sel(bank, i); // 0=A,1=B
                int st = dyn_get_ab(bank, i) ? 1 : 0;                  // 0=A,1=B
                int on = ledsel ? st : (!st);

                if (is_down) on = 0;
                if (on) led_on(i); else led_off(i);
                continue;
            }

            // default: guide
            if (is_down) led_off(i);
            else led_on(i);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void footswitch_start(void)
{
    // ✅ restore last bank (persisted)
    footswitch_set_bank((int)config_store_get_current_bank());

    xTaskCreatePinnedToCore(foot_task, "footswitch", 4096, NULL, 6, NULL, 1);
}
