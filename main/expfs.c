// ===== FILE: main/expfs.c =====
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "config_store.h"
#include "midi_actions.h"
#include "usb_midi_host.h"
#include "uart_midi_out.h"

#include "expfs.h"

static const char *TAG = "EXPFS";

// -------------------- EXP smoothing / curve --------------------
// เป้าหมาย:
// 1) ลด jitter (ค่านิ่งๆ ไม่แกว่ง +/-1)
// 2) ทำสเกลให้สัมพันธ์กับระยะเท้ามากขึ้น (ชดเชย pot แบบ log/ไม่เชิงเส้น)
#define EXP_SEND_THROTTLE_MS   (20)
#define EXP_SEND_STABLE_MS     (10)
#define EXP_IIR_SHIFT          (1)     // 2^3 = 8  (ยิ่งมากยิ่งเนียน)
#define EXP_FORCE_DELTA        (1)     // diff >= 4 ส่งทันที (รู้สึกตอบสนอง)
#define EXP_CURVE_GAMMA        (1.0f)  // 1.0 = linear LUT (ยังคง LUT ไว้เผื่อปรับภายหลัง)


// -------------------- pin map (ตามที่กำหนดให้) --------------------
typedef struct {
    gpio_num_t tip;
    gpio_num_t ring;
} expfs_hw_t;

static const expfs_hw_t HW[EXPFS_PORT_COUNT] = {
    { (gpio_num_t)15, (gpio_num_t)16 }, // EXP/FS #1
    { (gpio_num_t)1,  (gpio_num_t)2  }, // EXP/FS #2
};

// -------------------- ADC oneshot handles --------------------
static adc_oneshot_unit_handle_t s_adc_u1 = NULL;
static adc_oneshot_unit_handle_t s_adc_u2 = NULL;

typedef struct {
    int valid;
    adc_unit_t unit;
    adc_channel_t chan;
} adc_map_t;

static adc_map_t s_adc_map[EXPFS_PORT_COUNT];
static uint16_t  s_last_raw[EXPFS_PORT_COUNT];
static uint8_t   s_last_mapped[EXPFS_PORT_COUNT]; // last sent (0..127)
static uint32_t  s_last_send_ms[EXPFS_PORT_COUNT];

// exp runtime state (filter + stable send)
static uint16_t s_raw_hist[EXPFS_PORT_COUNT][3];
static uint8_t  s_raw_hist_idx[EXPFS_PORT_COUNT];
static int32_t  s_raw_filt[EXPFS_PORT_COUNT]; // filtered raw (0..4095)
static uint8_t  s_pending_mapped[EXPFS_PORT_COUNT];
static uint32_t s_pending_since_ms[EXPFS_PORT_COUNT];

static uint8_t  s_curve_lut[128];
static uint8_t  s_curve_inited;


// fs runtime state
static uint8_t s_fs_last_level[EXPFS_PORT_COUNT][2]; // [port][tip=0 ring=1] 0=pressed 1=released
static int     s_fs_hold_ms[EXPFS_PORT_COUNT][2];
static uint8_t s_fs_long_fired[EXPFS_PORT_COUNT][2];
static uint8_t s_fs_ab_state[EXPFS_PORT_COUNT][2];  // toggle a/b state (0=a 1=b)

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline int clampi_local(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t clamp7(int v)
{
    if (v < 0) return 0;
    if (v > 127) return 127;
    return (uint8_t)v;
}

static inline uint16_t median3_u16(uint16_t a, uint16_t b, uint16_t c)
{
    // median of 3
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;
}

static inline int iabs_local(int v) { return (v < 0) ? -v : v; }

static void exp_curve_init_once(void)
{
    if (s_curve_inited) return;
    for (int i = 0; i < 128; i++) {
        float x = (float)i / 127.0f;
        float y = powf(x, EXP_CURVE_GAMMA);
        int v = (int)lroundf(y * 127.0f);
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        s_curve_lut[i] = (uint8_t)v;
    }
    s_curve_lut[0] = 0;
    s_curve_lut[127] = 127;
    s_curve_inited = 1;
}

static inline int pressed_pin(gpio_num_t g)
{
    // pull-up: pressed = 0
    return gpio_get_level(g) == 0;
}

static void adc_init_once(void)
{
    static int inited = 0;
    if (inited) return;

    // prepare mapping for each port (ring pin used as ADC input)
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        s_adc_map[p].valid = 0;

        adc_unit_t unit;
        adc_channel_t ch;
        esp_err_t e = adc_oneshot_io_to_channel((int)HW[p].ring, &unit, &ch);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_io_to_channel failed port=%d ring=GPIO%d err=%s",
                     p, (int)HW[p].ring, esp_err_to_name(e));
            continue;
        }

        s_adc_map[p].valid = 1;
        s_adc_map[p].unit = unit;
        s_adc_map[p].chan = ch;
    }

    // create unit handles only if needed
    bool need_u1 = false, need_u2 = false;
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        if (!s_adc_map[p].valid) continue;
        if (s_adc_map[p].unit == ADC_UNIT_1) need_u1 = true;
        if (s_adc_map[p].unit == ADC_UNIT_2) need_u2 = true;
    }

    if (need_u1) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE };
        if (adc_oneshot_new_unit(&ucfg, &s_adc_u1) == ESP_OK) {
            ESP_LOGI(TAG, "ADC_UNIT_1 ready");
        }
    }
    if (need_u2) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_2, .ulp_mode = ADC_ULP_MODE_DISABLE };
        if (adc_oneshot_new_unit(&ucfg, &s_adc_u2) == ESP_OK) {
            ESP_LOGI(TAG, "ADC_UNIT_2 ready");
        }
    }

    // config channels
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        if (!s_adc_map[p].valid) continue;

        adc_oneshot_unit_handle_t h = NULL;
        if (s_adc_map[p].unit == ADC_UNIT_1) h = s_adc_u1;
        if (s_adc_map[p].unit == ADC_UNIT_2) h = s_adc_u2;
        if (!h) { s_adc_map[p].valid = 0; continue; }

        adc_oneshot_chan_cfg_t ccfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        (void)adc_oneshot_config_channel(h, s_adc_map[p].chan, &ccfg);
    }

    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        s_last_raw[p] = 0;
        s_last_mapped[p] = 0xFF;
        s_last_send_ms[p] = 0;
    }

    // exp filter init
    exp_curve_init_once();
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        s_raw_hist[p][0] = 0;
        s_raw_hist[p][1] = 0;
        s_raw_hist[p][2] = 0;
        s_raw_hist_idx[p] = 0;
        s_raw_filt[p] = 0;
        s_pending_mapped[p] = 0xFF;
        s_pending_since_ms[p] = 0;
    }

    // fs init
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        for (int k = 0; k < 2; k++) {
            s_fs_last_level[p][k] = 1;
            s_fs_hold_ms[p][k] = 0;
            s_fs_long_fired[p][k] = 0;
            s_fs_ab_state[p][k] = 0;
        }
    }

    inited = 1;
}

static int adc_read_raw_port(int port, int *out_raw)
{
    if (!out_raw) return 0;
    if (port < 0 || port >= EXPFS_PORT_COUNT) return 0;
    if (!s_adc_map[port].valid) return 0;

    adc_oneshot_unit_handle_t h = NULL;
    if (s_adc_map[port].unit == ADC_UNIT_1) h = s_adc_u1;
    if (s_adc_map[port].unit == ADC_UNIT_2) h = s_adc_u2;
    if (!h) return 0;

    int raw = 0;
    esp_err_t e = adc_oneshot_read(h, s_adc_map[port].chan, &raw);
    if (e != ESP_OK) return 0;

    *out_raw = raw;
    return 1;
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

static uint8_t map_exp_value(const expfs_port_cfg_t *cfg, uint16_t raw)
{
    if (!cfg) return 0;

    // calibration meaning (ตาม UI):
    // - cal_min = เหยียบลงสุด (toe)
    // - cal_max = ยกขึ้นสุด (heel)
    // ต้องการทิศทาง: "เหยียบลงค่าลด"  ✅
    // ดังนั้น: toe(down) -> val ต่ำ, heel(up) -> val สูง
    int lo = (int)cfg->cal_min; // down (toe)
    int hi = (int)cfg->cal_max; // up   (heel)
    int32_t denom = (int32_t)hi - (int32_t)lo;

    // avoid div0 / too small range
    if (denom > -8 && denom < 8) return 0;

    // clamp raw into [min(lo,hi), max(lo,hi)]
    int mn = (lo < hi) ? lo : hi;
    int mx = (lo < hi) ? hi : lo;
    int r = clampi_local((int)raw, mn, mx);

    // base normalize: lo -> 0, hi -> 127 (denom may be negative)
    int32_t num = (int32_t)r - (int32_t)lo;
    int norm127 = (int)((int64_t)num * 127LL / (int64_t)denom);
    norm127 = clampi_local(norm127, 0, 127);

    // apply curve LUT (ตอนนี้ gamma=1 -> linear)
    norm127 = (int)s_curve_lut[norm127];

    // IMPORTANT: invert direction so "down decreases"
    // after this:
    // - raw==lo (down)  => norm127 becomes 127
    // - raw==hi (up)    => norm127 becomes 0
    // and later mapping v1..v2 will make down -> lower output (as requested)
    norm127 = 127 - norm127;

    // output range val1..val2
    int v1 = 0, v2 = 127;
    if (cfg->exp_action.type == ACT_CC) {
        v1 = cfg->exp_action.b;
        v2 = cfg->exp_action.c;
    } else if (cfg->exp_action.type == ACT_PC) {
        v1 = cfg->exp_action.a;
        v2 = cfg->exp_action.b;
    }

    int out;
    if (v2 >= v1) out = v1 + (int)((int64_t)norm127 * (v2 - v1) / 127LL);
    else          out = v1 - (int)((int64_t)norm127 * (v1 - v2) / 127LL);

    return clamp7(out);
}

static void handle_exp_port(int port, const expfs_port_cfg_t *cfg)
{
    // EXP mode:
    // - TIP = 3.3V output high (Vref)
    // - RING = ADC input
    gpio_set_direction(HW[port].tip, GPIO_MODE_OUTPUT);
    gpio_set_level(HW[port].tip, 1);

    gpio_set_direction(HW[port].ring, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HW[port].ring, GPIO_FLOATING);

    int raw_i = 0;
    if (adc_read_raw_port(port, &raw_i)) {
        if (raw_i < 0) raw_i = 0;
        if (raw_i > 4095) raw_i = 4095;
        s_last_raw[port] = (uint16_t)raw_i;
    }

    // -------- median(3) + IIR filtering to reduce jitter --------
    uint16_t raw_u = s_last_raw[port];

    // prime history on first run
    if (s_raw_hist[port][0] == 0 && s_raw_hist[port][1] == 0 && s_raw_hist[port][2] == 0) {
        s_raw_hist[port][0] = raw_u;
        s_raw_hist[port][1] = raw_u;
        s_raw_hist[port][2] = raw_u;
        s_raw_filt[port] = (int32_t)raw_u;
    } else {
        uint8_t idx = s_raw_hist_idx[port];
        s_raw_hist[port][idx] = raw_u;
        s_raw_hist_idx[port] = (uint8_t)((idx + 1) % 3);

        uint16_t med = median3_u16(s_raw_hist[port][0], s_raw_hist[port][1], s_raw_hist[port][2]);
        int32_t f = s_raw_filt[port];
        f += ((int32_t)med - f) >> EXP_IIR_SHIFT;
        if (f < 0) f = 0;
        if (f > 4095) f = 4095;
        s_raw_filt[port] = f;
    }

    uint16_t raw_f = (uint16_t)s_raw_filt[port];

    // map (with curve) to output value
    uint8_t mapped = map_exp_value(cfg, raw_f);

    uint32_t t = now_ms();

    // stable window: ต้องนิ่งซักพักก่อนส่ง เพื่อตัดอาการแกว่ง +/-1
    if (mapped != s_pending_mapped[port]) {
        s_pending_mapped[port] = mapped;
        s_pending_since_ms[port] = t;
    }

    int last_sent = (int)s_last_mapped[port];
    int diff = (last_sent == 0xFF) ? 127 : iabs_local((int)mapped - last_sent);
    bool stable_ok = (t - s_pending_since_ms[port]) >= EXP_SEND_STABLE_MS;
    bool throttle_ok = (t - s_last_send_ms[port]) >= EXP_SEND_THROTTLE_MS;

    if (mapped != s_last_mapped[port] && throttle_ok && (stable_ok || diff >= EXP_FORCE_DELTA || s_last_mapped[port] == 0xFF)) {
        s_last_send_ms[port] = t;
        s_last_mapped[port] = mapped;

        uint8_t ch = (uint8_t)clampi_local((int)cfg->exp_action.ch, 1, 16);

        if (cfg->exp_action.type == ACT_CC) {
            uint8_t cc = clamp7(cfg->exp_action.a);
            send_cc_all(ch, cc, mapped);
        } else if (cfg->exp_action.type == ACT_PC) {
            send_pc_all(ch, mapped);
        }
    }
}

static void run_actions_trigger_list(const action_t *list, cc_behavior_t cc_beh)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, MIDI_EVT_TRIGGER);
}
static void run_actions_down_up_list(const action_t *list, cc_behavior_t cc_beh, int event)
{
    midi_actions_run(list, MAX_ACTIONS, cc_beh, event);
}

static void handle_fs_one(int port, int which /*0 tip, 1 ring*/, gpio_num_t pin, const expfs_btncfg_t *m)
{
    const int LONG_MS = 400;

    int now = gpio_get_level(pin); // 0 pressed, 1 released
    uint8_t last = s_fs_last_level[port][which];

    const action_t *listA = m->short_actions;
    const action_t *listB = m->long_actions;

    // edge down
    if (last == 1 && now == 0) {
        s_fs_hold_ms[port][which] = 0;
        s_fs_long_fired[port][which] = 0;

        // momentary down
        if (m->cc_behavior == CC_MOMENTARY) {
            if (m->press_mode == BTN_TOGGLE) {
                uint8_t st = s_fs_ab_state[port][which] ? 1 : 0;
                run_actions_down_up_list(st ? listB : listA, m->cc_behavior, MIDI_EVT_DOWN);
            } else {
                run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_DOWN);
            }
        }

        // toggle immediate
        if (m->press_mode == BTN_TOGGLE) {
            uint8_t st = s_fs_ab_state[port][which] ? 1 : 0;
            run_actions_trigger_list(st ? listB : listA, m->cc_behavior);
            s_fs_ab_state[port][which] = (uint8_t)!st;
        }
    }

    // hold
    if (now == 0) {
        s_fs_hold_ms[port][which] += 10;

        if (m->press_mode == BTN_SHORT_LONG &&
            !s_fs_long_fired[port][which] &&
            s_fs_hold_ms[port][which] >= LONG_MS)
        {
            run_actions_trigger_list(listB, m->cc_behavior);
            s_fs_long_fired[port][which] = 1;
        }
    }

    // edge up
    if (last == 0 && now == 1) {
        if (m->cc_behavior == CC_MOMENTARY) {
            if (m->press_mode == BTN_TOGGLE) {
                // use state BEFORE flip is ok; momentary expects "same selection"
                uint8_t st = s_fs_ab_state[port][which] ? 1 : 0;
                run_actions_down_up_list(st ? listB : listA, m->cc_behavior, MIDI_EVT_UP);
            } else {
                run_actions_down_up_list(listA, m->cc_behavior, MIDI_EVT_UP);
            }
        }

        if (m->press_mode == BTN_SHORT) {
            run_actions_trigger_list(listA, m->cc_behavior);
        }

        if (m->press_mode == BTN_SHORT_LONG) {
            if (!s_fs_long_fired[port][which] && s_fs_hold_ms[port][which] < 400) {
                run_actions_trigger_list(listA, m->cc_behavior);
            }
        }

        s_fs_hold_ms[port][which] = 0;
        s_fs_long_fired[port][which] = 0;
    }

    s_fs_last_level[port][which] = (uint8_t)now;
}

static void handle_fs_port(int port, const expfs_port_cfg_t *cfg)
{
    // FS mode: tip/ring are inputs w/ pull-up
    gpio_set_direction(HW[port].tip, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HW[port].tip, GPIO_PULLUP_ONLY);

    gpio_set_direction(HW[port].ring, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HW[port].ring, GPIO_PULLUP_ONLY);

    if (cfg->kind == EXPFS_KIND_SINGLE_SW) {
        handle_fs_one(port, 0, HW[port].tip, &cfg->tip);
    } else if (cfg->kind == EXPFS_KIND_DUAL_SW) {
        handle_fs_one(port, 0, HW[port].tip,  &cfg->tip);
        handle_fs_one(port, 1, HW[port].ring, &cfg->ring);
    }
}

static void expfs_task(void *arg)
{
    (void)arg;

    adc_init_once();

    // init GPIO levels cache
    for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
        s_fs_last_level[p][0] = 1;
        s_fs_last_level[p][1] = 1;
    }

    while (1) {
        const expfs_port_cfg_t *c0 = config_store_get_expfs_cfg(0);
        const expfs_port_cfg_t *c1 = config_store_get_expfs_cfg(1);

        const expfs_port_cfg_t *cfgs[2] = { c0, c1 };

        for (int p = 0; p < EXPFS_PORT_COUNT; p++) {
            const expfs_port_cfg_t *cfg = cfgs[p];
            if (!cfg) continue;

            if (cfg->kind == EXPFS_KIND_EXP) {
                handle_exp_port(p, cfg);
            } else {
                handle_fs_port(p, cfg);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void expfs_start(void)
{
    xTaskCreatePinnedToCore(expfs_task, "expfs", 4096, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "EXP/FS started (ports=%d)", EXPFS_PORT_COUNT);
}

uint16_t expfs_get_last_raw(int port)
{
    if (port < 0 || port >= EXPFS_PORT_COUNT) return 0;
    return s_last_raw[port];
}

esp_err_t expfs_cal_save(int port, int which_min0_max1)
{
    port = clampi_local(port, 0, EXPFS_PORT_COUNT - 1);
    uint16_t raw = expfs_get_last_raw(port);
    return config_store_set_expfs_cal(port, which_min0_max1 ? 1 : 0, raw);
}
