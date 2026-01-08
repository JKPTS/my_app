// ===== FILE: main/portal_wifi.c =====
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "cJSON.h"

#include "dns_hijack.h"
#include "config_store.h"
#include "footswitch.h"

static const char *TAG = "PORTAL";
static httpd_handle_t s_http = NULL;

// --- buffer size: prefer PSRAM, fallback small internal ---
#if CONFIG_SPIRAM
  #define BUF_MAX 16384
#else
  #define BUF_MAX 2048
#endif

static char *s_buf = NULL;
static SemaphoreHandle_t s_buf_lock = NULL;

static inline int wrapi(int v, int max)
{
    if (max <= 0) return 0;
    int r = v % max;
    if (r < 0) r += max;
    return r;
}

static inline int clampi_local(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool check_ok_or_ignore_invalid_state(esp_err_t e, const char *what)
{
    if (e == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "%s already initialized", what);
        return true;
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(e));
        return false;
    }
    return true;
}

// esp_http_server ไม่มี enum 503 → ต้องตั้ง status เอง
static esp_err_t resp_503(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg ? msg : "service unavailable");
    return ESP_OK;
}

static esp_err_t send_spiffs_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, ctype);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// -------- Static files --------
static esp_err_t h_root(httpd_req_t *req) { return send_spiffs_file(req, "/spiffs/index.html", "text/html"); }
static esp_err_t h_js(httpd_req_t *req)   { return send_spiffs_file(req, "/spiffs/app.js", "application/javascript"); }
static esp_err_t h_css(httpd_req_t *req)  { return send_spiffs_file(req, "/spiffs/style.css", "text/css"); }

// -------- Captive portal detection endpoints (redirect to /) --------
static esp_err_t h_redirect_to_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_generate_204(httpd_req_t *req) { return h_redirect_to_root(req); }
static esp_err_t h_hotspot(httpd_req_t *req)      { return h_redirect_to_root(req); }
static esp_err_t h_ncsi(httpd_req_t *req)         { return h_redirect_to_root(req); }

// -------- API: META --------
static esp_err_t h_get_meta(httpd_req_t *req)
{
    char out[220];
    int bc = config_store_bank_count();
    snprintf(out, sizeof(out),
             "{\"maxBanks\":%d,\"buttons\":%d,\"bankCount\":%d,\"maxActions\":%d,\"longMs\":%d}",
             MAX_BANKS, NUM_BTNS, bc, MAX_ACTIONS, 400);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// -------- API: LED (global brightness) --------
static esp_err_t h_get_led(httpd_req_t *req)
{
    uint8_t bri = config_store_get_led_brightness();
    char out[64];
    snprintf(out, sizeof(out), "{\"brightness\":%u}", (unsigned)bri);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t h_post_led(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char buf[257];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        got += r;
    }
    buf[total] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    cJSON *jb = cJSON_GetObjectItem(root, "brightness");
    if (!cJSON_IsNumber(jb)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json fields");
        return ESP_FAIL;
    }

    int bri = clampi_local(jb->valueint, 0, 100);
    cJSON_Delete(root);

    esp_err_t e = config_store_set_led_brightness((uint8_t)bri);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -------- API: LAYOUT (banks) --------
static esp_err_t h_get_layout(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    memset(s_buf, 0, BUF_MAX + 1);
    esp_err_t e = config_store_get_layout_json(s_buf, BUF_MAX);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "layout read failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_buf);
    return ESP_OK;
}

static esp_err_t h_post_layout(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    int total = req->content_len;
    if (total <= 0 || total > BUF_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, s_buf + got, total - got);
        if (r <= 0) {
            if (s_buf_lock) xSemaphoreGive(s_buf_lock);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        got += r;
    }
    s_buf[total] = 0;

    esp_err_t e = config_store_set_layout_json(s_buf);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "layout invalid");
        return ESP_FAIL;
    }

    // clamp bank after layout change
    footswitch_state_t st = footswitch_get_state();
    int bc = config_store_bank_count();
    int b = wrapi((int)st.bank, bc);
    footswitch_set_bank(b);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -------- API: BANK (switch names) --------
static esp_err_t h_get_bank(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    char q[96] = {0};
    int bank = 0;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char tmp[16];
        if (httpd_query_key_value(q, "bank", tmp, sizeof(tmp)) == ESP_OK) bank = atoi(tmp);
    }

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    memset(s_buf, 0, BUF_MAX + 1);
    esp_err_t e = config_store_get_bank_json(bank, s_buf, BUF_MAX);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bank read failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_buf);
    return ESP_OK;
}

static esp_err_t h_post_bank(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    char q[96] = {0};
    int bank = 0;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char tmp[16];
        if (httpd_query_key_value(q, "bank", tmp, sizeof(tmp)) == ESP_OK) bank = atoi(tmp);
    }

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);

    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, s_buf + got, total - got);
        if (r <= 0) {
            if (s_buf_lock) xSemaphoreGive(s_buf_lock);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        got += r;
    }
    s_buf[total] = 0;

    esp_err_t e = config_store_set_bank_json(bank, s_buf);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bank invalid");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -------- API: state (bank only) --------
static esp_err_t h_get_state(httpd_req_t *req)
{
    footswitch_state_t st = footswitch_get_state();
    char out[96];
    snprintf(out, sizeof(out), "{\"bank\":%u}", (unsigned)st.bank);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t h_post_state(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char buf[257];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        got += r;
    }
    buf[total] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    cJSON *jb = cJSON_GetObjectItem(root, "bank");
    if (!cJSON_IsNumber(jb)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json fields");
        return ESP_FAIL;
    }

    int bc = config_store_bank_count();
    int bank = wrapi(jb->valueint, bc);

    footswitch_set_bank(bank);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -------- API: per-button mapping --------
static esp_err_t h_get_button(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    char q[96] = {0};
    int bank = 0, btn = 0;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char tmp[16];
        if (httpd_query_key_value(q, "bank", tmp, sizeof(tmp)) == ESP_OK) bank = atoi(tmp);
        if (httpd_query_key_value(q, "btn",  tmp, sizeof(tmp)) == ESP_OK) btn  = atoi(tmp);
    }

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn,  NUM_BTNS);

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    memset(s_buf, 0, BUF_MAX + 1);
    esp_err_t e = config_store_get_btn_json(bank, btn, s_buf, BUF_MAX);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "button read failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_buf);
    return ESP_OK;
}

static esp_err_t h_post_button(httpd_req_t *req)
{
    if (!s_buf) return resp_503(req, "buffer not ready");

    char q[96] = {0};
    int bank = 0, btn = 0;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char tmp[16];
        if (httpd_query_key_value(q, "bank", tmp, sizeof(tmp)) == ESP_OK) bank = atoi(tmp);
        if (httpd_query_key_value(q, "btn",  tmp, sizeof(tmp)) == ESP_OK) btn  = atoi(tmp);
    }

    int bc = config_store_bank_count();
    bank = wrapi(bank, bc);
    btn  = wrapi(btn,  NUM_BTNS);

    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    if (s_buf_lock) xSemaphoreTake(s_buf_lock, portMAX_DELAY);

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, s_buf + got, total - got);
        if (r <= 0) {
            if (s_buf_lock) xSemaphoreGive(s_buf_lock);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        got += r;
    }
    s_buf[total] = 0;

    esp_err_t e = config_store_set_btn_json(bank, btn, s_buf);

    if (s_buf_lock) xSemaphoreGive(s_buf_lock);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "button config invalid");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---- helper: register with log ----
static void reg_uri(httpd_handle_t h, const httpd_uri_t *u, const char *name)
{
    esp_err_t e = httpd_register_uri_handler(h, u);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "register uri failed (%s) uri=%s method=%d err=%s",
                 (name ? name : "?"),
                 (u && u->uri) ? u->uri : "(null)",
                 (u ? (int)u->method : -1),
                 esp_err_to_name(e));
    }
}

static void start_http_server(void)
{
    if (s_http) {
        ESP_LOGW(TAG, "HTTP server already started");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.max_open_sockets = 2;
    cfg.stack_size = 4096;
    cfg.lru_purge_enable = true;

    esp_err_t e = httpd_start(&s_http, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        s_http = NULL;
        return;
    }

    httpd_uri_t u_root = { .uri="/", .method=HTTP_GET, .handler=h_root };
    httpd_uri_t u_js   = { .uri="/app.js", .method=HTTP_GET, .handler=h_js };
    httpd_uri_t u_css  = { .uri="/style.css", .method=HTTP_GET, .handler=h_css };

    httpd_uri_t u_204  = { .uri="/generate_204", .method=HTTP_GET, .handler=h_generate_204 };
    httpd_uri_t u_hot  = { .uri="/hotspot-detect.html", .method=HTTP_GET, .handler=h_hotspot };
    httpd_uri_t u_ncsi = { .uri="/ncsi.txt", .method=HTTP_GET, .handler=h_ncsi };
    httpd_uri_t u_ct   = { .uri="/connecttest.txt", .method=HTTP_GET, .handler=h_ncsi };

    httpd_uri_t u_meta   = { .uri="/api/meta",   .method=HTTP_GET,  .handler=h_get_meta };
    httpd_uri_t u_layout = { .uri="/api/layout", .method=HTTP_GET,  .handler=h_get_layout };
    httpd_uri_t u_pl     = { .uri="/api/layout", .method=HTTP_POST, .handler=h_post_layout };

    httpd_uri_t u_bank_g = { .uri="/api/bank",   .method=HTTP_GET,  .handler=h_get_bank };
    httpd_uri_t u_bank_p = { .uri="/api/bank",   .method=HTTP_POST, .handler=h_post_bank };

    httpd_uri_t u_gs   = { .uri="/api/state", .method=HTTP_GET,  .handler=h_get_state };
    httpd_uri_t u_ps   = { .uri="/api/state", .method=HTTP_POST, .handler=h_post_state };

    httpd_uri_t u_gb   = { .uri="/api/button", .method=HTTP_GET,  .handler=h_get_button };
    httpd_uri_t u_pb   = { .uri="/api/button", .method=HTTP_POST, .handler=h_post_button };

    httpd_uri_t u_led_g = { .uri="/api/led", .method=HTTP_GET,  .handler=h_get_led };
    httpd_uri_t u_led_p = { .uri="/api/led", .method=HTTP_POST, .handler=h_post_led };

    reg_uri(s_http, &u_root,  "root");
    reg_uri(s_http, &u_js,    "js");
    reg_uri(s_http, &u_css,   "css");

    reg_uri(s_http, &u_204,   "generate_204");
    reg_uri(s_http, &u_hot,   "hotspot");
    reg_uri(s_http, &u_ncsi,  "ncsi");
    reg_uri(s_http, &u_ct,    "connecttest");

    reg_uri(s_http, &u_meta,   "meta");
    reg_uri(s_http, &u_layout, "layout_get");
    reg_uri(s_http, &u_pl,     "layout_post");

    reg_uri(s_http, &u_bank_g, "bank_get");
    reg_uri(s_http, &u_bank_p, "bank_post");

    reg_uri(s_http, &u_gs, "state_get");
    reg_uri(s_http, &u_ps, "state_post");

    reg_uri(s_http, &u_gb, "button_get");
    reg_uri(s_http, &u_pb, "button_post");

    reg_uri(s_http, &u_led_g, "led_get");
    reg_uri(s_http, &u_led_p, "led_post");

    ESP_LOGI(TAG, "HTTP server started");
}

static bool mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "SPIFFS mounted");
    return true;
}

void portal_wifi_start(void)
{
    if (!s_buf_lock) {
        s_buf_lock = xSemaphoreCreateMutex();
        if (!s_buf_lock) {
            ESP_LOGE(TAG, "mutex alloc failed");
            return;
        }
    }

    if (!check_ok_or_ignore_invalid_state(esp_netif_init(), "esp_netif_init")) return;
    if (!check_ok_or_ignore_invalid_state(esp_event_loop_create_default(), "esp_event_loop_create_default")) return;

    esp_netif_create_default_wifi_ap();

    ESP_LOGI(TAG, "heap before wifi_init: free=%u, 8bit=%u, internal=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t we = esp_wifi_init(&wicfg);

    if (we == ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGW(TAG, "esp_wifi_init already done");
    } else if (we != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(we));
        ESP_LOGE(TAG, "heap at fail: free=%u, 8bit=%u, internal=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }

    wifi_config_t ap = { 0 };
    const char *ssid = "FOOTSWITCH-SETUP";
    const char *pass = "12345678";

    strncpy((char*)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    strncpy((char*)ap.ap.password, pass, sizeof(ap.ap.password) - 1);

    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen((char*)ap.ap.password) == 0) ap.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_AP);
    if (e != ESP_OK) { ESP_LOGE(TAG, "wifi_set_mode failed: %s", esp_err_to_name(e)); return; }

    e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e != ESP_OK) { ESP_LOGE(TAG, "wifi_set_config failed: %s", esp_err_to_name(e)); return; }

    e = esp_wifi_start();
    if (e != ESP_OK) { ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(e)); return; }

    if (!s_buf) {
#if CONFIG_SPIRAM
        s_buf = (char*)heap_caps_malloc(BUF_MAX + 1, MALLOC_CAP_SPIRAM);
        if (s_buf) {
            ESP_LOGI(TAG, "portal buffer allocated in PSRAM: %d bytes", BUF_MAX);
        }
#endif
        if (!s_buf) {
            s_buf = (char*)heap_caps_malloc(BUF_MAX + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_buf) {
                ESP_LOGW(TAG, "portal buffer allocated in INTERNAL: %d bytes", BUF_MAX);
            }
        }
        if (!s_buf) {
            ESP_LOGE(TAG, "portal buffer alloc failed");
        } else {
            s_buf[0] = 0;
        }
    }

    (void)mount_spiffs();

    dns_hijack_start();
    start_http_server();

    if (!s_http) {
        ESP_LOGW(TAG, "Portal running WITHOUT HTTP server (low heap). Enable PSRAM / reduce Wi-Fi buffers.");
    }

    ESP_LOGI(TAG, "Captive portal ready. Connect Wi-Fi 'FOOTSWITCH-SETUP' then open 192.168.4.1");
}
