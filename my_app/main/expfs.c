// ===== FILE: main/expfs.c =====
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "esp_adc/adc_oneshot.h"

#include "expfs.h"
#include "config_store.h"
#include "midi_actions.h"

static const char *TAG = "EXPFS";

// -------------------- pins --------------------
// Port1: tip=GPIO1 (ADC), ring=GPIO16
// Port2: tip=GPIO2 (ADC), ring=GPIO15
#define EXPFS_PORTS 2

static const gpio_num_t TIP_GPIO[EXPFS_PORTS]  = { (gpio_num_t)1,  (gpio_num_t)2  };
static const gpio_num_t RING_GPIO[EXPFS_PORTS] = { (gpio_num_t)16, (gpio_num_t)15 };

// ADC: assume GPIO1=ADC1_CH0, GPIO2=ADC1_CH1 (ESP32-S3)
static const adc_channel_t TIP_ADC_CH[EXPFS_PORTS] = { ADC_CHANNEL_0, ADC_CHANNEL_1 };
static const adc_unit_t TIP_ADC_UNIT = ADC_UNIT_1;

// -------------------- helpers --------------------
static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void run_actions_trigger_list(const action_t *list, cc_behavior_t cc_beh)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, MIDI_EVT_TRIGGER);
}

static void run_actions_down_up_list(const action_t *list, cc_behavior_t cc_beh, int event)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, event);
}

// -------------------- adc --------------------
static adc_oneshot_unit_handle_t s_adc = NULL;

static bool adc_init_once(void)
{
    if (s_adc) return true;

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = TIP_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t e = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (e != ESP_OK || !s_adc) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(e));
        s_adc = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    for (int p = 0; p < EXPFS_PORTS; p++) {
        e = adc_oneshot_config_channel(s_adc, TIP_ADC_CH[p], &ccfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_config_channel port=%d failed: %s", p, esp_err_to_name(e));
        }
    }

    ESP_LOGI(TAG, "ADC oneshot ready");
    return true;
}

static int adc_read_raw(int port)
{
    if (!s_adc) return -1;
    port = clampi(port, 0, EXPFS_PORTS - 1);

    int raw = 0;
    esp_err_t e = adc_oneshot_read(s_adc, TIP_ADC_CH[port], &raw);
    if (e != ESP_OK) return -1;
    return raw;
}

// map raw(0..4095) -> midi(0..127)
static uint8_t raw_to_midi(int raw)
{
    raw = clampi(raw, 0, 4095);
    int v = (raw * 127 + 2047) / 4095;
    return (uint8_t)clampi(v, 0, 127);
}

// -------------------- per switch state --------------------
typedef struct {
    uint8_t last_level;   // 1=released, 0=pressed
    int hold_ms;
    uint8_t long_fired;
    uint8_t toggle_ab;    // 0=A,1=B (for pressMode=TOGGLE)
    uint8_t pressed_sel;  // for momentary toggle: remember A/B at press down
} sw_state_t;

typedef struct {
    // exp
    uint8_t last_midi;
    uint8_t exp_inited;

    // switch
    sw_state_t tip;
    sw_state_t ring;
} port_state_t;

static port_state_t s_ps[EXPFS_PORTS];

// -------------------- gpio mode configure --------------------
static void cfg_pin_input_pu(gpio_num_t pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void cfg_pin_input_nopull(gpio_num_t pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void cfg_pin_output_high(gpio_num_t pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, 1);
}

static uint8_t read_level(gpio_num_t pin)
{
    return (uint8_t)(gpio_get_level(pin) ? 1 : 0);
}

static void apply_port_gpio_mode(int port, const expfs_port_cfg_t *cfg)
{
    if (!cfg) return;
    port = clampi(port, 0, EXPFS_PORTS - 1);

    const gpio_num_t tip  = TIP_GPIO[port];
    const gpio_num_t ring = RING_GPIO[port];

    // safe default
    cfg_pin_input_pu(tip);
    cfg_pin_input_pu(ring);

    if (cfg->kind == EXPFS_KIND_EXP) {
        // ring becomes Vref (3.3V), tip is ADC input (no pull)
        cfg_pin_output_high(ring);
        cfg_pin_input_nopull(tip);
        (void)adc_init_once();
        return;
    }

    if (cfg->kind == EXPFS_KIND_SINGLE_SW) {
        // tip used as switch to GND (pull-up), ring unused (keep pull-up input)
        cfg_pin_input_pu(tip);
        cfg_pin_input_pu(ring);
        return;
    }

    if (cfg->kind == EXPFS_KIND_DUAL_SW) {
        // tip + ring both switches to GND (pull-up)
        cfg_pin_input_pu(tip);
        cfg_pin_input_pu(ring);
        return;
    }
}

// -------------------- switch engine --------------------
static void sw_handle_one(sw_state_t *st,
                          gpio_num_t pin,
                          const expfs_btncfg_t *m,
                          int long_ms)
{
    if (!st || !m) return;

    uint8_t now = read_level(pin); // 0 pressed, 1 released (because pull-up)
    const action_t *listA = m->short_actions;
    const action_t *listB = m->long_actions;

    // edge: down
    if (st->last_level == 1 && now == 0) {
        st->hold_ms = 0;
        st->long_fired = 0;

        // momentary: DOWN
        if (m->cc_behavior == CC_MOMENTARY) {
            if (m->press_mode == BTN_TOGGLE) {
                uint8_t sel = st->toggle_ab ? 1 : 0;
                st->pressed_sel = sel;
                run_actions_down_up_list(sel ? listB : listA, m->cc_behavior, MIDI_EVT_DOWN);
            } else {
                run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_DOWN);
            }
        }

        // toggle: trigger + flip
        if (m->press_mode == BTN_TOGGLE) {
            uint8_t sel = st->toggle_ab ? 1 : 0;
            run_actions_trigger_list(sel ? listB : listA, m->cc_behavior);
            st->toggle_ab = (uint8_t)!st->toggle_ab;
        }
    }

    // hold
    if (now == 0) {
        st->hold_ms += 10;

        if (m->press_mode == BTN_SHORT_LONG && !st->long_fired && st->hold_ms >= long_ms) {
            run_actions_trigger_list(listB, m->cc_behavior);
            st->long_fired = 1;
        }
    }

    // edge: up
    if (st->last_level == 0 && now == 1) {
        if (m->cc_behavior == CC_MOMENTARY) {
            if (m->press_mode == BTN_TOGGLE) {
                uint8_t sel = st->pressed_sel ? 1 : 0;
                run_actions_down_up_list(sel ? listB : listA, m->cc_behavior, MIDI_EVT_UP);
            } else {
                run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_UP);
            }
        }

        if (m->press_mode == BTN_SHORT) {
            run_actions_trigger_list(listA, m->cc_behavior);
        } else if (m->press_mode == BTN_SHORT_LONG) {
            if (!st->long_fired && st->hold_ms < long_ms) {
                run_actions_trigger_list(listA, m->cc_behavior);
            }
        } else {
            // BTN_TOGGLE already handled on down
        }

        st->hold_ms = 0;
        st->long_fired = 0;
    }

    st->last_level = now;
}

// -------------------- exp engine --------------------
static void exp_handle_one(int port, port_state_t *ps, const expfs_port_cfg_t *cfg)
{
    if (!ps || !cfg) return;
    if (cfg->kind != EXPFS_KIND_EXP) return;

    // only one command
    // cfg->exp_action must be ACT_CC
    if (cfg->exp_action.type != ACT_CC) return;

    int raw = adc_read_raw(port);
    if (raw < 0) return;

    uint8_t v = raw_to_midi(raw);

    if (!ps->exp_inited) {
        ps->exp_inited = 1;
        ps->last_midi = v;
        // send initial to set baseline
        action_t a = cfg->exp_action;
        a.b = v; // value comes from pedal
        midi_actions_run(&a, 1, CC_NORMAL, MIDI_EVT_TRIGGER);
        return;
    }

    if (v == ps->last_midi) return;

    ps->last_midi = v;

    action_t a = cfg->exp_action;
    a.b = v;
    midi_actions_run(&a, 1, CC_NORMAL, MIDI_EVT_TRIGGER);
}

// -------------------- task --------------------
static void expfs_task(void *arg)
{
    (void)arg;

    // init defaults for state
    memset(s_ps, 0, sizeof(s_ps));
    for (int p = 0; p < EXPFS_PORTS; p++) {
        s_ps[p].tip.last_level  = 1;
        s_ps[p].ring.last_level = 1;
        s_ps[p].tip.toggle_ab   = 0;
        s_ps[p].ring.toggle_ab  = 0;
        s_ps[p].exp_inited      = 0;
        s_ps[p].last_midi       = 0;
    }

    expfs_port_cfg_t last_cfg[EXPFS_PORTS];
    memset(last_cfg, 0, sizeof(last_cfg));

    // apply GPIO mode once at boot
    for (int p = 0; p < EXPFS_PORTS; p++) {
        const expfs_port_cfg_t *cfgp = config_store_get_expfs_cfg(p);
        if (!cfgp) continue;
        last_cfg[p] = *cfgp;
        apply_port_gpio_mode(p, cfgp);
    }

    const int LONG_MS = 400;

    while (1) {
        for (int p = 0; p < EXPFS_PORTS; p++) {
            const expfs_port_cfg_t *cfgp = config_store_get_expfs_cfg(p);
            if (!cfgp) continue;

            // if config kind changed -> re-apply gpio mode + reset exp init
            if (cfgp->kind != last_cfg[p].kind) {
                last_cfg[p].kind = cfgp->kind;
                apply_port_gpio_mode(p, cfgp);
                s_ps[p].exp_inited = 0;
            }

            // EXP
            exp_handle_one(p, &s_ps[p], cfgp);

            // SWITCH
            if (cfgp->kind == EXPFS_KIND_SINGLE_SW) {
                // no group led allowed; (enforced in config_store sanitize)
                sw_handle_one(&s_ps[p].tip, TIP_GPIO[p], &cfgp->tip, LONG_MS);

            } else if (cfgp->kind == EXPFS_KIND_DUAL_SW) {
                sw_handle_one(&s_ps[p].tip,  TIP_GPIO[p],  &cfgp->tip,  LONG_MS);
                sw_handle_one(&s_ps[p].ring, RING_GPIO[p], &cfgp->ring, LONG_MS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void expfs_start(void)
{
    xTaskCreatePinnedToCore(expfs_task, "expfs", 4096, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "exp/fs started (ports=%d)", EXPFS_PORTS);
}
