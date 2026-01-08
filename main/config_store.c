// ===== FILE: main/config_store.c =====
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>  // snprintf

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include "config_store.h"

static const char *TAG = "CFG";

/**
 * ✅ DRAM overflow guard:
 * - ใช้ heap allocate (PSRAM ก่อน) แทน static ใหญ่ ๆ ใน .bss
 */
static foot_config_t *s_cfg = NULL;

// ✅ สถานะ NVS (กัน abort/รีบูต)
static bool s_nvs_ok = false;

// ---- led brightness stored separately ----
static uint8_t s_led_brightness = 100; // 0..100

// ---- a+b led selection stored separately ----
// 0=A, 1=B
static uint8_t s_ab_led_sel[MAX_BANKS][NUM_BTNS];

// ---- current bank persisted ----
static uint8_t s_cur_bank = 0;

// ---- exp/fs stored separately (blob) ----
static expfs_port_cfg_t s_expfs[EXPFS_PORT_COUNT];

#define CFG_MAGIC 0x46435346u  // 'FSCF'
#define CFG_VER   4            // v4 = no pages

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t ver;
    uint16_t reserved;
    uint32_t size;
} cfg_hdr_v4_t;

// ---------- legacy structures (v3 had pages) ----------
#define LEGACY_V3_MAX_BANKS 20
#define LEGACY_V3_MAX_PAGES 4
#define LEGACY_V3_NUM_BTNS  8

typedef struct {
    action_type_t type;
    uint8_t ch;
    uint8_t a;
    uint8_t b;
    uint8_t c;
} legacy_action_t;

typedef struct {
    btn_press_mode_t press_mode;
    cc_behavior_t cc_behavior;
    legacy_action_t short_actions[MAX_ACTIONS];
    legacy_action_t long_actions[MAX_ACTIONS];
} legacy_btn_map_t;

typedef struct {
    uint8_t bank_count;
    uint8_t page_count[LEGACY_V3_MAX_BANKS];
    char    bank_name[LEGACY_V3_MAX_BANKS][NAME_LEN];
    char    page_name[LEGACY_V3_MAX_BANKS][LEGACY_V3_MAX_PAGES][NAME_LEN];
    char    switch_name[LEGACY_V3_MAX_BANKS][LEGACY_V3_MAX_PAGES][LEGACY_V3_NUM_BTNS][NAME_LEN];
    legacy_btn_map_t map[LEGACY_V3_MAX_BANKS][LEGACY_V3_MAX_PAGES][LEGACY_V3_NUM_BTNS];
} legacy_foot_config_v3_t;

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int wrapi(int v, int max)
{
    if (max <= 0) return 0;
    int r = v % max;
    if (r < 0) r += max;
    return r;
}

static void set_default_action(action_t *a)
{
    if (!a) return;
    a->type = ACT_NONE;
    a->ch   = 1;
    a->a    = 0;
    a->b    = 0;
    a->c    = 0;
}

static void safe_set_name(char dst[NAME_LEN], const char *src, const char *fallback)
{
    const char *s = (src && src[0]) ? src : fallback;
    if (!s) s = "";
    strncpy(dst, s, NAME_LEN - 1);
    dst[NAME_LEN - 1] = 0;
}

// ---- led brightness NVS helpers ----
static esp_err_t nvs_load_led_brightness(uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    uint8_t v = 0;
    e = nvs_get_u8(h, "led_bri", &v);
    nvs_close(h);

    if (e != ESP_OK) return e;

    if (v > 100) v = 100;
    *out = v;
    return ESP_OK;
}

static esp_err_t nvs_save_led_brightness(uint8_t v)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    if (v > 100) v = 100;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_u8(h, "led_bri", v);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_save_led_brightness failed: %s", esp_err_to_name(e));
    return e;
}

// ---- current bank NVS helpers ----
static esp_err_t nvs_load_cur_bank(uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    uint8_t v = 0;
    e = nvs_get_u8(h, "cur_bank", &v);
    nvs_close(h);
    if (e != ESP_OK) return e;

    *out = v;
    return ESP_OK;
}

static esp_err_t nvs_save_cur_bank(uint8_t bank)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_u8(h, "cur_bank", bank);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_save_cur_bank failed: %s", esp_err_to_name(e));
    return e;
}

// ---- a+b led select NVS helpers (blob) ----
static void ab_led_defaults(void)
{
    // default = 1 (B) to match old behavior (A=off, B=on)
    memset(s_ab_led_sel, 1, sizeof(s_ab_led_sel));
}

static void ab_led_sanitize(void)
{
    for (int b = 0; b < MAX_BANKS; b++) {
        for (int k = 0; k < NUM_BTNS; k++) {
            s_ab_led_sel[b][k] = (s_ab_led_sel[b][k] ? 1u : 0u);
        }
    }
}

static esp_err_t nvs_load_ab_led_sel(void)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t len = 0;
    e = nvs_get_blob(h, "ab_led", NULL, &len);
    if (e != ESP_OK) {
        nvs_close(h);
        return e;
    }

    // new size
    if (len == sizeof(s_ab_led_sel)) {
        e = nvs_get_blob(h, "ab_led", s_ab_led_sel, &len);
        nvs_close(h);
        if (e == ESP_OK) ab_led_sanitize();
        return e;
    }

    // legacy size (20*4*8)
    const size_t legacy_len = (size_t)LEGACY_V3_MAX_BANKS * (size_t)LEGACY_V3_MAX_PAGES * (size_t)LEGACY_V3_NUM_BTNS;
    if (len == legacy_len) {
        uint8_t *tmp = (uint8_t *)malloc(len);
        if (!tmp) { nvs_close(h); return ESP_ERR_NO_MEM; }

        e = nvs_get_blob(h, "ab_led", tmp, &len);
        nvs_close(h);
        if (e != ESP_OK) { free(tmp); return e; }

        // migrate: take page0 only
        ab_led_defaults();
        for (int b = 0; b < LEGACY_V3_MAX_BANKS; b++) {
            for (int k = 0; k < LEGACY_V3_NUM_BTNS; k++) {
                size_t idx = (size_t)((b * LEGACY_V3_MAX_PAGES + 0) * LEGACY_V3_NUM_BTNS + k);
                s_ab_led_sel[b][k] = (tmp[idx] ? 1u : 0u);
            }
        }
        free(tmp);
        ab_led_sanitize();
        return ESP_OK;
    }

    nvs_close(h);
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t nvs_save_ab_led_sel(void)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_blob(h, "ab_led", s_ab_led_sel, sizeof(s_ab_led_sel));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_save_ab_led_sel failed: %s", esp_err_to_name(e));
    return e;
}

// -------------------- exp/fs helpers --------------------
static void expfs_set_defaults_one(expfs_port_cfg_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));

    p->kind = EXPFS_KIND_SINGLE_SW;

    // exp default = CC ch1 cc0 val1=0 val2=127
    p->exp_action.type = ACT_CC;
    p->exp_action.ch = 1;
    p->exp_action.a = 0;     // cc#
    p->exp_action.b = 0;     // val1
    p->exp_action.c = 100;   // val2

    // default calibration (safe)
    p->cal_min = 0;
    p->cal_max = 4095;

    // switch defaults
    p->tip.press_mode = BTN_SHORT;
    p->tip.cc_behavior = CC_NORMAL;
    for (int i = 0; i < MAX_ACTIONS; i++) {
        set_default_action(&p->tip.short_actions[i]);
        set_default_action(&p->tip.long_actions[i]);
    }

    p->ring.press_mode = BTN_SHORT;
    p->ring.cc_behavior = CC_NORMAL;
    for (int i = 0; i < MAX_ACTIONS; i++) {
        set_default_action(&p->ring.short_actions[i]);
        set_default_action(&p->ring.long_actions[i]);
    }
}

static void expfs_defaults(void)
{
    for (int i = 0; i < EXPFS_PORT_COUNT; i++) expfs_set_defaults_one(&s_expfs[i]);
}

static void expfs_sanitize_btn(expfs_btncfg_t *m)
{
    if (!m) return;

    // no group led for exp/fs (force into 0..2)
    int pm = (int)m->press_mode;
    if (pm == BTN_SHORT_GROUP_LED) pm = BTN_SHORT;
    m->press_mode = (btn_press_mode_t)clampi(pm, 0, 2);

    m->cc_behavior = (cc_behavior_t)clampi((int)m->cc_behavior, 0, 2);

    for (int i = 0; i < MAX_ACTIONS; i++) {
        action_t *sa = &m->short_actions[i];
        action_t *la = &m->long_actions[i];

        if (sa->type != ACT_CC && sa->type != ACT_PC) set_default_action(sa);
        if (la->type != ACT_CC && la->type != ACT_PC) set_default_action(la);

        sa->ch = (uint8_t)clampi((int)sa->ch, 1, 16);
        la->ch = (uint8_t)clampi((int)la->ch, 1, 16);
        sa->a  = (uint8_t)clampi((int)sa->a, 0, 127);
        sa->b  = (uint8_t)clampi((int)sa->b, 0, 127);
        sa->c  = 0;
        la->a  = (uint8_t)clampi((int)la->a, 0, 127);
        la->b  = (uint8_t)clampi((int)la->b, 0, 127);
        la->c  = 0;
    }
}

static void expfs_sanitize_all(void)
{
    for (int i = 0; i < EXPFS_PORT_COUNT; i++) {
        expfs_port_cfg_t *p = &s_expfs[i];

        p->kind = (expfs_kind_t)clampi((int)p->kind, 0, 2);

        // calibration sanity
        p->cal_min = (uint16_t)clampi((int)p->cal_min, 0, 4095);
        p->cal_max = (uint16_t)clampi((int)p->cal_max, 0, 4095);

        // exp action: allow CC or PC, single cmd
        if (p->exp_action.type != ACT_CC && p->exp_action.type != ACT_PC) set_default_action(&p->exp_action);
        p->exp_action.ch = (uint8_t)clampi((int)p->exp_action.ch, 1, 16);

        if (p->exp_action.type == ACT_CC) {
            p->exp_action.a = (uint8_t)clampi((int)p->exp_action.a, 0, 127); // cc#
            p->exp_action.b = (uint8_t)clampi((int)p->exp_action.b, 0, 127); // val1
            p->exp_action.c = (uint8_t)clampi((int)p->exp_action.c, 0, 127); // val2
        } else if (p->exp_action.type == ACT_PC) {
            // PC uses a=val1, b=val2
            p->exp_action.a = (uint8_t)clampi((int)p->exp_action.a, 0, 127);
            p->exp_action.b = (uint8_t)clampi((int)p->exp_action.b, 0, 127);
            p->exp_action.c = 0;
        } else {
            set_default_action(&p->exp_action);
        }

        expfs_sanitize_btn(&p->tip);
        expfs_sanitize_btn(&p->ring);
    }
}

static esp_err_t nvs_load_expfs(void)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t len = 0;
    e = nvs_get_blob(h, "expfs", NULL, &len);
    if (e != ESP_OK) { nvs_close(h); return e; }

    if (len != sizeof(s_expfs)) { nvs_close(h); return ESP_ERR_INVALID_SIZE; }

    e = nvs_get_blob(h, "expfs", s_expfs, &len);
    nvs_close(h);

    if (e == ESP_OK) expfs_sanitize_all();
    return e;
}

static esp_err_t nvs_save_expfs(void)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_blob(h, "expfs", s_expfs, sizeof(s_expfs));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_save_expfs failed: %s", esp_err_to_name(e));
    return e;
}

static void sanitize_cfg(foot_config_t *cfg)
{
    if (!cfg) return;

    for (int b = 0; b < MAX_BANKS; b++) {
        for (int k = 0; k < NUM_BTNS; k++) {
            btn_map_t *m = &cfg->map[b][k];

            for (int i = 0; i < MAX_ACTIONS; i++) {
                action_t *sa = &m->short_actions[i];
                action_t *la = &m->long_actions[i];

                if (sa->type != ACT_CC && sa->type != ACT_PC) set_default_action(sa);
                if (la->type != ACT_CC && la->type != ACT_PC) set_default_action(la);

                sa->ch = (uint8_t)clampi((int)sa->ch, 1, 16);
                la->ch = (uint8_t)clampi((int)la->ch, 1, 16);
                sa->a  = (uint8_t)clampi((int)sa->a, 0, 127);
                sa->b  = (uint8_t)clampi((int)sa->b, 0, 127);
                sa->c  = 0;
                la->a  = (uint8_t)clampi((int)la->a, 0, 127);
                la->b  = (uint8_t)clampi((int)la->b, 0, 127);
                la->c  = 0;
            }

            int pm = (int)m->press_mode;
            if (pm == 4) pm = 0; // migrate old tap tempo -> short
            m->press_mode  = (btn_press_mode_t)clampi(pm, 0, 3);
            m->cc_behavior = (cc_behavior_t)clampi((int)m->cc_behavior, 0, 2);

            cfg->switch_name[b][k][NAME_LEN - 1] = 0;
        }
        cfg->bank_name[b][NAME_LEN - 1] = 0;
    }

    cfg->bank_count = (uint8_t)clampi((int)cfg->bank_count, 1, MAX_BANKS);
}

static void set_defaults(foot_config_t *cfg)
{
    if (!cfg) return;

    memset(cfg, 0, sizeof(*cfg));

    cfg->bank_count = 1;

    for (int b = 0; b < MAX_BANKS; b++) {
        char bn[NAME_LEN];
        snprintf(bn, sizeof(bn), "Bank %d", b + 1);
        safe_set_name(cfg->bank_name[b], bn, "Bank");

        for (int k = 0; k < NUM_BTNS; k++) {
            char sn[NAME_LEN];
            snprintf(sn, sizeof(sn), "SW %d", k + 1);
            safe_set_name(cfg->switch_name[b][k], sn, "SW");

            btn_map_t *m = &cfg->map[b][k];
            m->press_mode  = BTN_SHORT;
            m->cc_behavior = CC_NORMAL;

            for (int i = 0; i < MAX_ACTIONS; i++) {
                set_default_action(&m->short_actions[i]);
                set_default_action(&m->long_actions[i]);
            }
        }
    }

    ab_led_defaults();
    s_cur_bank = 0;

    // exp/fs defaults too
    expfs_defaults();
}

// ---------- NVS load/save (v4) ----------
static esp_err_t nvs_load_v4(foot_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t hlen = sizeof(cfg_hdr_v4_t);
    cfg_hdr_v4_t hdr;
    e = nvs_get_blob(h, "cfg_hdr", &hdr, &hlen);
    if (e != ESP_OK || hlen != sizeof(cfg_hdr_v4_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    if (hdr.magic != CFG_MAGIC || hdr.ver != CFG_VER || hdr.size != sizeof(foot_config_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    size_t dlen = 0;
    e = nvs_get_blob(h, "cfg_data", NULL, &dlen);
    if (e != ESP_OK || dlen != sizeof(foot_config_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    e = nvs_get_blob(h, "cfg_data", out, &dlen);
    nvs_close(h);
    return e;
}

static void migrate_v3_to_v4(const legacy_foot_config_v3_t *old, foot_config_t *out)
{
    set_defaults(out);

    int old_bc = clampi((int)old->bank_count, 1, LEGACY_V3_MAX_BANKS);
    out->bank_count = (uint8_t)clampi(old_bc, 1, MAX_BANKS);

    for (int b = 0; b < old_bc; b++) {
        safe_set_name(out->bank_name[b], old->bank_name[b], out->bank_name[b]);

        for (int k = 0; k < NUM_BTNS; k++) {
            // take page0 only
            safe_set_name(out->switch_name[b][k], old->switch_name[b][0][k], out->switch_name[b][k]);

            const legacy_btn_map_t *om = &old->map[b][0][k];
            btn_map_t *nm = &out->map[b][k];

            nm->press_mode  = (btn_press_mode_t)clampi((int)om->press_mode, 0, 3);
            nm->cc_behavior = (cc_behavior_t)clampi((int)om->cc_behavior, 0, 2);

            for (int i = 0; i < MAX_ACTIONS; i++) {
                // short
                const legacy_action_t *osa = &om->short_actions[i];
                action_t *nsa = &nm->short_actions[i];
                nsa->type = osa->type;
                nsa->ch = osa->ch;
                nsa->a = osa->a;
                nsa->b = osa->b;
                nsa->c = 0;

                // long
                const legacy_action_t *ola = &om->long_actions[i];
                action_t *nla = &nm->long_actions[i];
                nla->type = ola->type;
                nla->ch = ola->ch;
                nla->a = ola->a;
                nla->b = ola->b;
                nla->c = 0;
            }
        }
    }
}

static esp_err_t nvs_load_migrate_v3_to_v4(foot_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t hlen = sizeof(cfg_hdr_v4_t);
    cfg_hdr_v4_t hdr;
    e = nvs_get_blob(h, "cfg_hdr", &hdr, &hlen);
    if (e != ESP_OK || hlen != sizeof(cfg_hdr_v4_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    // detect legacy v3 layout
    if (hdr.magic != CFG_MAGIC || hdr.ver != 3 || hdr.size != (uint32_t)sizeof(legacy_foot_config_v3_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    size_t dlen = 0;
    e = nvs_get_blob(h, "cfg_data", NULL, &dlen);
    if (e != ESP_OK || dlen != sizeof(legacy_foot_config_v3_t)) {
        nvs_close(h);
        return ESP_FAIL;
    }

    legacy_foot_config_v3_t *tmp = (legacy_foot_config_v3_t *)malloc(sizeof(*tmp));
    if (!tmp) { nvs_close(h); return ESP_ERR_NO_MEM; }

    e = nvs_get_blob(h, "cfg_data", tmp, &dlen);
    nvs_close(h);
    if (e != ESP_OK) { free(tmp); return e; }

    migrate_v3_to_v4(tmp, out);
    free(tmp);
    return ESP_OK;
}

static esp_err_t nvs_save_v4(const foot_config_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;

    cfg_hdr_v4_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CFG_MAGIC;
    hdr.ver   = CFG_VER;
    hdr.size  = (uint32_t)sizeof(foot_config_t);

    nvs_handle_t h;
    esp_err_t e = nvs_open("footsw", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_blob(h, "cfg_hdr", &hdr, sizeof(hdr));
    if (e == ESP_OK) e = nvs_set_blob(h, "cfg_data", in, sizeof(*in));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);

    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_save_v4 failed: %s", esp_err_to_name(e));
    return e;
}

const foot_config_t *config_store_get(void)
{
    return s_cfg;
}

void config_store_init(void)
{
    // ✅ allocate config first (prefer PSRAM)
    if (!s_cfg) {
        s_cfg = (foot_config_t *)heap_caps_malloc(sizeof(foot_config_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cfg) {
            s_cfg = (foot_config_t *)heap_caps_malloc(sizeof(foot_config_t), MALLOC_CAP_8BIT);
        }

        if (!s_cfg) {
            ESP_LOGE(TAG, "No heap for config_store (%u bytes). System will run without config.",
                     (unsigned)sizeof(foot_config_t));
            s_led_brightness = 100;
            ab_led_defaults();
            s_cur_bank = 0;
            expfs_defaults();
            return;
        }
    }

    // ✅ NVS init แบบไม่ทำให้รีบูต
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_INVALID_STATE) {
        s_nvs_ok = true;
        e = ESP_OK;
    } else if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (e=%s)", esp_err_to_name(e));
        esp_err_t e2 = nvs_flash_erase();
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(e2));
        }
        e = nvs_flash_init();
        s_nvs_ok = (e == ESP_OK);
    } else {
        s_nvs_ok = (e == ESP_OK);
    }

    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "NVS not available: %s (run with defaults, no persistence)", esp_err_to_name(e));
    }

    // defaults
    set_defaults(s_cfg);

    if (s_nvs_ok) {
        // v4 first
        e = nvs_load_v4(s_cfg);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "Loaded config v4 from NVS");
        } else {
            // migrate v3 -> v4 (page0)
            e = nvs_load_migrate_v3_to_v4(s_cfg);
            if (e == ESP_OK) {
                ESP_LOGW(TAG, "Migrated legacy v3 -> v4 (page removed, keep page0)");
                (void)nvs_save_v4(s_cfg);
            } else {
                ESP_LOGW(TAG, "No saved config (v4/v3), using defaults");
                (void)nvs_save_v4(s_cfg);
            }
        }

        sanitize_cfg(s_cfg);
        (void)nvs_save_v4(s_cfg);

        // led brightness
        uint8_t bri = 100;
        e = nvs_load_led_brightness(&bri);
        if (e == ESP_OK) {
            s_led_brightness = bri;
            ESP_LOGI(TAG, "Loaded led brightness=%u", (unsigned)s_led_brightness);
        } else {
            s_led_brightness = 100;
            (void)nvs_save_led_brightness(s_led_brightness);
            ESP_LOGW(TAG, "No led brightness saved, default=100");
        }

        // ab led sel
        ab_led_defaults();
        e = nvs_load_ab_led_sel();
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "Loaded ab led sel (blob)");
        } else {
            ab_led_defaults();
            (void)nvs_save_ab_led_sel();
            ESP_LOGW(TAG, "No ab led sel saved, default=B");
        }

        // current bank
        uint8_t cb = 0;
        e = nvs_load_cur_bank(&cb);
        if (e == ESP_OK) {
            int bc = config_store_bank_count();
            s_cur_bank = (uint8_t)wrapi((int)cb, bc);
            ESP_LOGI(TAG, "Loaded cur_bank=%u", (unsigned)s_cur_bank);
        } else {
            s_cur_bank = 0;
            (void)nvs_save_cur_bank(s_cur_bank);
            ESP_LOGW(TAG, "No cur_bank saved, default=0");
        }

        // exp/fs
        expfs_defaults();
        e = nvs_load_expfs();
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "Loaded exp/fs (blob)");
        } else {
            expfs_defaults();
            (void)nvs_save_expfs();
            ESP_LOGW(TAG, "No exp/fs saved, default=single sw");
        }

    } else {
        s_led_brightness = 100;
        ab_led_defaults();
        s_cur_bank = 0;
        expfs_defaults();
        sanitize_cfg(s_cfg);
        expfs_sanitize_all();
    }
}

// ---- helpers ----
int config_store_bank_count(void)
{
    if (!s_cfg) return 1;
    return (int)clampi((int)s_cfg->bank_count, 1, MAX_BANKS);
}

const char *config_store_bank_name(int bank)
{
    if (!s_cfg) return "Bank";
    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    return s_cfg->bank_name[bank];
}

// ---- layout json (banks only) ----
esp_err_t config_store_get_layout_json(char *out, int out_len)
{
    if (!out || out_len <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "maxBanks", MAX_BANKS);

    int bc = config_store_bank_count();
    cJSON_AddNumberToObject(root, "bankCount", bc);

    cJSON *banks = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "banks", banks);

    for (int b = 0; b < bc; b++) {
        cJSON *bo = cJSON_CreateObject();
        cJSON_AddNumberToObject(bo, "index", b);
        cJSON_AddStringToObject(bo, "name", s_cfg->bank_name[b]);
        cJSON_AddItemToArray(banks, bo);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;

    int need = (int)strlen(s);
    if (need >= out_len) {
        free(s);
        return ESP_FAIL;
    }

    strcpy(out, s);
    free(s);
    return ESP_OK;
}

esp_err_t config_store_set_layout_json(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *jbc = cJSON_GetObjectItem(root, "bankCount");
    cJSON *jbanks = cJSON_GetObjectItem(root, "banks");
    if (!cJSON_IsNumber(jbc) || !cJSON_IsArray(jbanks)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int bc = clampi(jbc->valueint, 1, MAX_BANKS);

    uint8_t new_bank_count = (uint8_t)bc;
    char new_bank_name[MAX_BANKS][NAME_LEN];
    memcpy(new_bank_name, s_cfg->bank_name, sizeof(new_bank_name));

    for (int b = 0; b < bc; b++) {
        cJSON *bo = cJSON_GetArrayItem(jbanks, b);
        if (!cJSON_IsObject(bo)) { cJSON_Delete(root); return ESP_FAIL; }

        cJSON *bn = cJSON_GetObjectItem(bo, "name");
        if (cJSON_IsString(bn)) safe_set_name(new_bank_name[b], bn->valuestring, new_bank_name[b]);
        else safe_set_name(new_bank_name[b], NULL, new_bank_name[b]);

        new_bank_name[b][NAME_LEN - 1] = 0;
    }

    cJSON_Delete(root);

    s_cfg->bank_count = new_bank_count;
    memcpy(s_cfg->bank_name, new_bank_name, sizeof(new_bank_name));

    sanitize_cfg(s_cfg);

    // clamp current bank if bankCount reduced
    int cur = (int)s_cur_bank;
    int bc2 = config_store_bank_count();
    s_cur_bank = (uint8_t)wrapi(cur, bc2);
    if (s_nvs_ok) (void)nvs_save_cur_bank(s_cur_bank);

    return nvs_save_v4(s_cfg);
}

// ---- bank json (switch names) ----
esp_err_t config_store_get_bank_json(int bank, char *out, int out_len)
{
    if (!out || out_len <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "switchNames", arr);

    for (int k = 0; k < NUM_BTNS; k++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_cfg->switch_name[bank][k]));
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;

    int need = (int)strlen(s);
    if (need >= out_len) {
        free(s);
        return ESP_FAIL;
    }

    strcpy(out, s);
    free(s);
    return ESP_OK;
}

esp_err_t config_store_set_bank_json(int bank, const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);

    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *arr = cJSON_GetObjectItem(root, "switchNames");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > NUM_BTNS) n = NUM_BTNS;

    for (int k = 0; k < n; k++) {
        cJSON *s = cJSON_GetArrayItem(arr, k);
        if (cJSON_IsString(s)) {
            safe_set_name(s_cfg->switch_name[bank][k], s->valuestring, s_cfg->switch_name[bank][k]);
        }
        s_cfg->switch_name[bank][k][NAME_LEN - 1] = 0;
    }

    cJSON_Delete(root);
    sanitize_cfg(s_cfg);
    return nvs_save_v4(s_cfg);
}

// ---------- JSON helpers (per-button) ----------
static bool parse_action(cJSON *o, action_t *a)
{
    if (!cJSON_IsObject(o) || !a) return false;

    const cJSON *type = cJSON_GetObjectItem(o, "type");
    const cJSON *ch   = cJSON_GetObjectItem(o, "ch");
    const cJSON *aa   = cJSON_GetObjectItem(o, "a");
    const cJSON *bb   = cJSON_GetObjectItem(o, "b");
    const cJSON *cc   = cJSON_GetObjectItem(o, "c"); // optional

    if (!cJSON_IsString(type) || !cJSON_IsNumber(ch) ||
        !cJSON_IsNumber(aa)   || !cJSON_IsNumber(bb)) {
        return false;
    }

    a->ch = (uint8_t)clampi((int)ch->valueint, 1, 16);

    int cv = 0;
    if (cJSON_IsNumber(cc)) cv = cc->valueint;

    if (strcmp(type->valuestring, "cc") == 0) {
        a->type = ACT_CC;
        a->a = (uint8_t)clampi((int)aa->valueint, 0, 127);
        a->b = (uint8_t)clampi((int)bb->valueint, 0, 127);
        a->c = (uint8_t)clampi(cv, 0, 127);
        return true;
    }

    if (strcmp(type->valuestring, "pc") == 0) {
        a->type = ACT_PC;
        a->a = (uint8_t)clampi((int)aa->valueint, 0, 127);
        a->b = (uint8_t)clampi((int)bb->valueint, 0, 127); // used by EXP (val2) / otherwise 0
        a->c = 0;
        return true;
    }

    return false;
}

static void action_to_json(cJSON *arr, const action_t *a)
{
    if (!arr || !a) return;
    if (a->type == ACT_NONE) return;

    const char *t = NULL;
    if (a->type == ACT_CC) t = "cc";
    if (a->type == ACT_PC) t = "pc";
    if (!t) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", t);
    cJSON_AddNumberToObject(o, "ch", a->ch);
    cJSON_AddNumberToObject(o, "a",  a->a);
    cJSON_AddNumberToObject(o, "b",  a->b);
    cJSON_AddNumberToObject(o, "c",  a->c);
    cJSON_AddItemToArray(arr, o);
}

esp_err_t config_store_get_btn_json(int bank, int btn, char *out, int out_len)
{
    if (!out || out_len <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn,  NUM_BTNS);

    const btn_map_t *m = &s_cfg->map[bank][btn];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "pressMode",  (int)m->press_mode);
    cJSON_AddNumberToObject(root, "ccBehavior", (int)m->cc_behavior);
    cJSON_AddNumberToObject(root, "abLed", (int)(s_ab_led_sel[bank][btn] ? 1 : 0));

    cJSON *sa = cJSON_CreateArray();
    cJSON *la = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "short", sa);
    cJSON_AddItemToObject(root, "long",  la);

    for (int i = 0; i < MAX_ACTIONS; i++) action_to_json(sa, &m->short_actions[i]);
    for (int i = 0; i < MAX_ACTIONS; i++) action_to_json(la, &m->long_actions[i]);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;

    int need = (int)strlen(s);
    if (need >= out_len) {
        free(s);
        return ESP_FAIL;
    }

    strcpy(out, s);
    free(s);
    return ESP_OK;
}

esp_err_t config_store_set_btn_json(int bank, int btn, const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_cfg) return ESP_FAIL;

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn,  NUM_BTNS);

    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *pm = cJSON_GetObjectItem(root, "pressMode");
    cJSON *cb = cJSON_GetObjectItem(root, "ccBehavior");
    cJSON *sa = cJSON_GetObjectItem(root, "short");
    cJSON *la = cJSON_GetObjectItem(root, "long");
    cJSON *ab = cJSON_GetObjectItem(root, "abLed");

    if (!cJSON_IsNumber(pm) || !cJSON_IsNumber(cb) || !cJSON_IsArray(sa) || !cJSON_IsArray(la)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    btn_map_t *m = &s_cfg->map[bank][btn];

    int pressMode = clampi(pm->valueint, 0, 3);
    int ccBeh     = clampi(cb->valueint, 0, 2);
    m->press_mode  = (btn_press_mode_t)pressMode;
    m->cc_behavior = (cc_behavior_t)ccBeh;

    if (cJSON_IsNumber(ab)) {
        int sel = clampi(ab->valueint, 0, 1);
        s_ab_led_sel[bank][btn] = (uint8_t)sel;
    } else {
        s_ab_led_sel[bank][btn] = (s_ab_led_sel[bank][btn] ? 1u : 0u);
    }

    for (int i = 0; i < MAX_ACTIONS; i++) {
        set_default_action(&m->short_actions[i]);
        set_default_action(&m->long_actions[i]);
    }

    int ns = cJSON_GetArraySize(sa);
    if (ns > MAX_ACTIONS) ns = MAX_ACTIONS;
    for (int i = 0; i < ns; i++) {
        if (!parse_action(cJSON_GetArrayItem(sa, i), &m->short_actions[i])) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
    }

    int nl = cJSON_GetArraySize(la);
    if (nl > MAX_ACTIONS) nl = MAX_ACTIONS;
    for (int i = 0; i < nl; i++) {
        if (!parse_action(cJSON_GetArrayItem(la, i), &m->long_actions[i])) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    sanitize_cfg(s_cfg);

    esp_err_t e = nvs_save_v4(s_cfg);
    if (e != ESP_OK) return e;
    (void)nvs_save_ab_led_sel();
    return ESP_OK;
}

// ---- led brightness public API ----
uint8_t config_store_get_led_brightness(void)
{
    return s_led_brightness;
}

esp_err_t config_store_set_led_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_led_brightness = percent;
    return nvs_save_led_brightness(s_led_brightness);
}

// ---- a+b led selection public API ----
uint8_t config_store_get_ab_led_sel(int bank, int btn)
{
    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn, NUM_BTNS);

    return (s_ab_led_sel[bank][btn] ? 1u : 0u);
}

esp_err_t config_store_set_ab_led_sel(int bank, int btn, uint8_t sel)
{
    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn, NUM_BTNS);

    sel = (sel ? 1u : 0u);
    s_ab_led_sel[bank][btn] = sel;
    return nvs_save_ab_led_sel();
}

// ---- current bank persistence public API ----
uint8_t config_store_get_current_bank(void)
{
    int bc = config_store_bank_count();
    return (uint8_t)wrapi((int)s_cur_bank, bc);
}

esp_err_t config_store_set_current_bank(uint8_t bank)
{
    int bc = config_store_bank_count();
    s_cur_bank = (uint8_t)wrapi((int)bank, bc);
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    return nvs_save_cur_bank(s_cur_bank);
}

// -------------------- exp/fs JSON API --------------------
const expfs_port_cfg_t *config_store_get_expfs_cfg(int port)
{
    port = clampi(port, 0, EXPFS_PORT_COUNT - 1);
    return &s_expfs[port];
}

static const char *kind_to_str(expfs_kind_t k)
{
    if (k == EXPFS_KIND_EXP) return "exp";
    if (k == EXPFS_KIND_SINGLE_SW) return "single";
    if (k == EXPFS_KIND_DUAL_SW) return "dual";
    return "single";
}

static expfs_kind_t str_to_kind(const char *s)
{
    if (!s) return EXPFS_KIND_SINGLE_SW;
    if (strcmp(s, "exp") == 0) return EXPFS_KIND_EXP;
    if (strcmp(s, "single") == 0) return EXPFS_KIND_SINGLE_SW;
    if (strcmp(s, "dual") == 0) return EXPFS_KIND_DUAL_SW;
    return EXPFS_KIND_SINGLE_SW;
}

static void btncfg_to_json(cJSON *root, const expfs_btncfg_t *m)
{
    cJSON_AddNumberToObject(root, "pressMode", (int)m->press_mode);
    cJSON_AddNumberToObject(root, "ccBehavior", (int)m->cc_behavior);

    cJSON *sa = cJSON_CreateArray();
    cJSON *la = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "short", sa);
    cJSON_AddItemToObject(root, "long", la);

    for (int i = 0; i < MAX_ACTIONS; i++) action_to_json(sa, &m->short_actions[i]);
    for (int i = 0; i < MAX_ACTIONS; i++) action_to_json(la, &m->long_actions[i]);
}

static bool json_to_btncfg(cJSON *root, expfs_btncfg_t *m)
{
    if (!cJSON_IsObject(root) || !m) return false;

    cJSON *pm = cJSON_GetObjectItem(root, "pressMode");
    cJSON *cb = cJSON_GetObjectItem(root, "ccBehavior");
    cJSON *sa = cJSON_GetObjectItem(root, "short");
    cJSON *la = cJSON_GetObjectItem(root, "long");

    if (!cJSON_IsNumber(pm) || !cJSON_IsNumber(cb) || !cJSON_IsArray(sa) || !cJSON_IsArray(la)) return false;

    int pressMode = clampi(pm->valueint, 0, 2);
    int ccBeh     = clampi(cb->valueint, 0, 2);

    m->press_mode = (btn_press_mode_t)pressMode;
    m->cc_behavior = (cc_behavior_t)ccBeh;

    for (int i = 0; i < MAX_ACTIONS; i++) {
        set_default_action(&m->short_actions[i]);
        set_default_action(&m->long_actions[i]);
    }

    int ns = cJSON_GetArraySize(sa);
    if (ns > MAX_ACTIONS) ns = MAX_ACTIONS;
    for (int i = 0; i < ns; i++) {
        if (!parse_action(cJSON_GetArrayItem(sa, i), &m->short_actions[i])) return false;
    }

    int nl = cJSON_GetArraySize(la);
    if (nl > MAX_ACTIONS) nl = MAX_ACTIONS;
    for (int i = 0; i < nl; i++) {
        if (!parse_action(cJSON_GetArrayItem(la, i), &m->long_actions[i])) return false;
    }

    expfs_sanitize_btn(m);
    return true;
}

esp_err_t config_store_get_expfs_json(int port, char *out, int out_len)
{
    if (!out || out_len <= 0) return ESP_ERR_INVALID_ARG;
    port = clampi(port, 0, EXPFS_PORT_COUNT - 1);

    const expfs_port_cfg_t *p = &s_expfs[port];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "kind", kind_to_str(p->kind));
    cJSON_AddNumberToObject(root, "calMin", (int)p->cal_min);
    cJSON_AddNumberToObject(root, "calMax", (int)p->cal_max);

    // exp
    cJSON *exp = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "exp", exp);
    cJSON *expArr = cJSON_CreateArray();
    cJSON_AddItemToObject(exp, "cmd", expArr);
    action_to_json(expArr, &p->exp_action);

    // tip/ring
    cJSON *tip = cJSON_CreateObject();
    cJSON *ring = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "tip", tip);
    cJSON_AddItemToObject(root, "ring", ring);

    btncfg_to_json(tip, &p->tip);
    btncfg_to_json(ring, &p->ring);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;

    int need = (int)strlen(s);
    if (need >= out_len) { free(s); return ESP_FAIL; }

    strcpy(out, s);
    free(s);
    return ESP_OK;
}

esp_err_t config_store_set_expfs_json(int port, const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    port = clampi(port, 0, EXPFS_PORT_COUNT - 1);

    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;

    cJSON *jk = cJSON_GetObjectItem(root, "kind");
    if (!cJSON_IsString(jk)) { cJSON_Delete(root); return ESP_FAIL; }

    expfs_port_cfg_t tmp;
    expfs_set_defaults_one(&tmp);
    tmp.kind = str_to_kind(jk->valuestring);

    // calibration
    cJSON *jmin = cJSON_GetObjectItem(root, "calMin");
    cJSON *jmax = cJSON_GetObjectItem(root, "calMax");
    if (cJSON_IsNumber(jmin)) tmp.cal_min = (uint16_t)clampi(jmin->valueint, 0, 4095);
    if (cJSON_IsNumber(jmax)) tmp.cal_max = (uint16_t)clampi(jmax->valueint, 0, 4095);

    // exp cmd (single item)
    cJSON *jexp = cJSON_GetObjectItem(root, "exp");
    if (cJSON_IsObject(jexp)) {
        cJSON *cmd = cJSON_GetObjectItem(jexp, "cmd");
        if (cJSON_IsArray(cmd) && cJSON_GetArraySize(cmd) > 0) {
            action_t a;
            set_default_action(&a);
            if (parse_action(cJSON_GetArrayItem(cmd, 0), &a)) {
                // allow CC or PC only
                if (a.type == ACT_CC) {
                    // keep a=cc#, b=val1, c=val2
                    tmp.exp_action = a;
                } else if (a.type == ACT_PC) {
                    // PC uses a=val1 b=val2
                    a.c = 0;
                    tmp.exp_action = a;
                }
            }
        }
    }

    // tip/ring cfg
    cJSON *jtip  = cJSON_GetObjectItem(root, "tip");
    cJSON *jring = cJSON_GetObjectItem(root, "ring");

    if (cJSON_IsObject(jtip)) {
        if (!json_to_btncfg(jtip, &tmp.tip)) { cJSON_Delete(root); return ESP_FAIL; }
    }
    if (cJSON_IsObject(jring)) {
        if (!json_to_btncfg(jring, &tmp.ring)) { cJSON_Delete(root); return ESP_FAIL; }
    }

    cJSON_Delete(root);

    // store then sanitize all
    s_expfs[port] = tmp;
    expfs_sanitize_all();

    if (s_nvs_ok) return nvs_save_expfs();
    return ESP_ERR_INVALID_STATE;
}

esp_err_t config_store_set_expfs_cal(int port, int which_min0_max1, uint16_t raw)
{
    port = clampi(port, 0, EXPFS_PORT_COUNT - 1);
    raw = (uint16_t)clampi((int)raw, 0, 4095);

    if (which_min0_max1) s_expfs[port].cal_max = raw;
    else s_expfs[port].cal_min = raw;

    expfs_sanitize_all();
    if (s_nvs_ok) return nvs_save_expfs();
    return ESP_ERR_INVALID_STATE;
}
