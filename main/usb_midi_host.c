// ===== FILE: main/usb_midi_host.c =====
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

#include "usb_midi_host.h"

static const char *TAG = "USB_MIDI";

#define SEND_ALL_CABLES 0

typedef struct {
    usb_host_client_handle_t client_hdl;
    usb_device_handle_t dev_hdl;

    bool have_device;
    bool claimed;
    uint8_t dev_addr;

    uint8_t midi_intf_num;
    uint8_t midi_ep_out;

    usb_transfer_t *xfer;
    SemaphoreHandle_t tx_done_sem;
} usb_midi_host_state_t;

static usb_midi_host_state_t s_usb;

// event flags from callback -> handled in client task
static volatile bool s_evt_new_dev = false;
static volatile bool s_evt_dev_gone = false;
static volatile uint8_t s_evt_new_addr = 0;

// Minimal header for walking descriptors
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
} usb_desc_header_t;

// -------------------- Transfer callback --------------------
static void transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        // ok
    } else {
        ESP_LOGW(TAG, "TX status=%d", (int)transfer->status);
    }
    if (s_usb.tx_done_sem) xSemaphoreGive(s_usb.tx_done_sem);
}

// -------------------- USB client event callback --------------------
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_evt_new_addr = event_msg->new_dev.address;
        s_evt_new_dev = true;
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_evt_dev_gone = true;
    }
}

// -------------------- Find MIDI streaming interface + OUT endpoint --------------------
static bool find_midi_out_ep(const usb_config_desc_t *cfg, uint8_t *out_intf, uint8_t *out_ep)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;

    const usb_intf_desc_t *cur_intf = NULL;

    uint8_t found_intf = 0;
    uint8_t found_ep   = 0;
    bool found_bulk = false;

    while (p + sizeof(usb_desc_header_t) <= end) {
        const usb_desc_header_t *hdr = (const usb_desc_header_t *)p;
        if (hdr->bLength == 0) break;
        if (p + hdr->bLength > end) break;

        if (hdr->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            cur_intf = (const usb_intf_desc_t *)p;

            // MIDI Streaming = Audio class(0x01), subclass(0x03)
            if (cur_intf->bInterfaceClass == 0x01 && cur_intf->bInterfaceSubClass == 0x03) {
                // keep
            } else {
                cur_intf = NULL;
            }
        } else if (hdr->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cur_intf) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;

            uint8_t xfer_type = (ep->bmAttributes & 0x03);
            bool is_bulk      = (xfer_type == 0x02);
            bool is_interrupt = (xfer_type == 0x03);
            bool is_out       = ((ep->bEndpointAddress & 0x80) == 0x00);

            if (is_out && (is_bulk || is_interrupt)) {
                if (is_bulk) {
                    *out_intf = cur_intf->bInterfaceNumber;
                    *out_ep   = ep->bEndpointAddress;
                    return true;
                }
                if (!found_bulk) {
                    found_bulk = false;
                    found_intf = cur_intf->bInterfaceNumber;
                    found_ep   = ep->bEndpointAddress;
                }
            }
        }
        p += hdr->bLength;
    }

    if (found_ep) {
        *out_intf = found_intf;
        *out_ep   = found_ep;
        return true;
    }
    return false;
}

static inline uint8_t clamp_ch(uint8_t ch_1_16)
{
    if (ch_1_16 < 1) return 1;
    if (ch_1_16 > 16) return 16;
    return ch_1_16;
}

// USB-MIDI event packet (4 bytes): [0]=(Cable<<4)|CIN [1]=status [2]=d1 [3]=d2
static inline void build_pkt_3b(uint8_t *pkt, uint8_t cable, uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2)
{
    pkt[0] = (uint8_t)(((cable & 0x0F) << 4) | (cin & 0x0F));
    pkt[1] = status;
    pkt[2] = d1;
    pkt[3] = d2;
}
static inline void build_pkt_2b(uint8_t *pkt, uint8_t cable, uint8_t cin, uint8_t status, uint8_t d1)
{
    pkt[0] = (uint8_t)(((cable & 0x0F) << 4) | (cin & 0x0F));
    pkt[1] = status;
    pkt[2] = d1;
    pkt[3] = 0x00;
}
static inline void build_pkt_1b(uint8_t *pkt, uint8_t cable, uint8_t cin, uint8_t b0)
{
    pkt[0] = (uint8_t)(((cable & 0x0F) << 4) | (cin & 0x0F));
    pkt[1] = b0;
    pkt[2] = 0x00;
    pkt[3] = 0x00;
}

static void midi_close_device(void)
{
    if (!s_usb.dev_hdl) {
        s_usb.have_device = false;
        s_usb.claimed = false;
        s_usb.midi_ep_out = 0;
        s_usb.midi_intf_num = 0;
        if (s_usb.tx_done_sem) xSemaphoreGive(s_usb.tx_done_sem);
        return;
    }

    if (s_usb.midi_ep_out) {
        (void)usb_host_endpoint_halt(s_usb.dev_hdl, s_usb.midi_ep_out);
        (void)usb_host_endpoint_flush(s_usb.dev_hdl, s_usb.midi_ep_out);
    }

    if (s_usb.claimed) {
        (void)usb_host_interface_release(s_usb.client_hdl, s_usb.dev_hdl, s_usb.midi_intf_num);
        s_usb.claimed = false;
    }

    (void)usb_host_device_close(s_usb.client_hdl, s_usb.dev_hdl);
    s_usb.dev_hdl = NULL;

    s_usb.have_device = false;
    s_usb.midi_ep_out = 0;
    s_usb.midi_intf_num = 0;

    if (s_usb.xfer) {
        s_usb.xfer->device_handle = NULL;
        s_usb.xfer->bEndpointAddress = 0;
    }

    if (s_usb.tx_done_sem) xSemaphoreGive(s_usb.tx_done_sem);
}

static esp_err_t ensure_midi_ready(void)
{
    if (!s_usb.have_device) return ESP_ERR_INVALID_STATE;

    if (s_usb.dev_hdl == NULL) {
        esp_err_t e = usb_host_device_open(s_usb.client_hdl, s_usb.dev_addr, &s_usb.dev_hdl);
        if (e != ESP_OK) return e;

        const usb_config_desc_t *cfg_desc = NULL;
        e = usb_host_get_active_config_descriptor(s_usb.dev_hdl, &cfg_desc);
        if (e != ESP_OK) { midi_close_device(); return e; }

        uint8_t intf = 0, ep_out = 0;
        if (!find_midi_out_ep(cfg_desc, &intf, &ep_out)) {
            ESP_LOGE(TAG, "No MIDI OUT endpoint found");
            midi_close_device();
            return ESP_FAIL;
        }

        s_usb.midi_intf_num = intf;
        s_usb.midi_ep_out = ep_out;

        e = usb_host_interface_claim(s_usb.client_hdl, s_usb.dev_hdl, s_usb.midi_intf_num, 0);
        if (e != ESP_OK) { midi_close_device(); return e; }
        s_usb.claimed = true;

        if (s_usb.xfer == NULL) {
            e = usb_host_transfer_alloc(64, 0, &s_usb.xfer);
            if (e != ESP_OK) { midi_close_device(); return e; }
            s_usb.xfer->callback = transfer_cb;
            s_usb.xfer->context = NULL;
        }

        s_usb.xfer->device_handle = s_usb.dev_hdl;
        s_usb.xfer->bEndpointAddress = s_usb.midi_ep_out;
    }

    return ESP_OK;
}

int usb_midi_ready_fast(void)
{
    return (s_usb.have_device &&
            s_usb.dev_hdl != NULL &&
            s_usb.claimed &&
            s_usb.xfer != NULL &&
            s_usb.midi_ep_out != 0);
}

static esp_err_t submit_pkt(const uint8_t pkt4[4])
{
    if (ensure_midi_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    if (s_usb.tx_done_sem) xSemaphoreTake(s_usb.tx_done_sem, pdMS_TO_TICKS(1000));

    memcpy(s_usb.xfer->data_buffer, pkt4, 4);
    s_usb.xfer->num_bytes = 4;

    esp_err_t err = usb_host_transfer_submit(s_usb.xfer);
    if (err != ESP_OK) {
        if (s_usb.tx_done_sem) xSemaphoreGive(s_usb.tx_done_sem);
    }
    return err;
}

esp_err_t usb_midi_send_cc(uint8_t ch_1_16, uint8_t cc, uint8_t val)
{
    ch_1_16 = clamp_ch(ch_1_16);

#if SEND_ALL_CABLES
    esp_err_t last = ESP_OK;
    for (uint8_t cable = 0; cable < 16; cable++) {
        uint8_t pkt[4];
        build_pkt_3b(pkt, cable, 0x0B, (uint8_t)(0xB0 | ((ch_1_16 - 1) & 0x0F)), (uint8_t)(cc & 0x7F), (uint8_t)(val & 0x7F));
        last = submit_pkt(pkt);
        if (last != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return last;
#else
    uint8_t pkt[4];
    build_pkt_3b(pkt, 0, 0x0B, (uint8_t)(0xB0 | ((ch_1_16 - 1) & 0x0F)), (uint8_t)(cc & 0x7F), (uint8_t)(val & 0x7F));
    return submit_pkt(pkt);
#endif
}

esp_err_t usb_midi_send_pc(uint8_t ch_1_16, uint8_t pc)
{
    ch_1_16 = clamp_ch(ch_1_16);

    uint8_t pkt[4];
    // CIN 0x0C = Program Change (2 bytes)
    build_pkt_2b(pkt, 0, 0x0C, (uint8_t)(0xC0 | ((ch_1_16 - 1) & 0x0F)), (uint8_t)(pc & 0x7F));
    return submit_pkt(pkt);
}

esp_err_t usb_midi_send_note_on(uint8_t ch_1_16, uint8_t note, uint8_t vel)
{
    ch_1_16 = clamp_ch(ch_1_16);

    uint8_t pkt[4];
    // CIN 0x09 = Note On (3 bytes)
    build_pkt_3b(pkt, 0, 0x09, (uint8_t)(0x90 | ((ch_1_16 - 1) & 0x0F)), (uint8_t)(note & 0x7F), (uint8_t)(vel & 0x7F));
    return submit_pkt(pkt);
}

esp_err_t usb_midi_send_note_off(uint8_t ch_1_16, uint8_t note, uint8_t vel)
{
    ch_1_16 = clamp_ch(ch_1_16);

    uint8_t pkt[4];
    // CIN 0x08 = Note Off (3 bytes)
    build_pkt_3b(pkt, 0, 0x08, (uint8_t)(0x80 | ((ch_1_16 - 1) & 0x0F)), (uint8_t)(note & 0x7F), (uint8_t)(vel & 0x7F));
    return submit_pkt(pkt);
}

// ✅ realtime: CIN 0x0F = single byte (system real-time เช่น F8 clock)
esp_err_t usb_midi_send_rt(uint8_t rt_byte)
{
    uint8_t pkt[4];
    build_pkt_1b(pkt, 0, 0x0F, rt_byte);
    return submit_pkt(pkt);
}

// -------------------- tasks --------------------
static void usb_host_daemon_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events err=%s", esp_err_to_name(err));
        }
    }
}

static void usb_client_task(void *arg)
{
    (void)arg;

    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t e = usb_host_client_register(&client_cfg, &s_usb.client_hdl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB client registered");

    while (1) {
        usb_host_client_handle_events(s_usb.client_hdl, pdMS_TO_TICKS(20));

        if (s_evt_dev_gone) {
            s_evt_dev_gone = false;
            ESP_LOGW(TAG, "DEV_GONE");
            midi_close_device();
        }

        if (s_evt_new_dev) {
            s_evt_new_dev = false;
            s_usb.dev_addr = s_evt_new_addr;
            s_usb.have_device = true;
            ESP_LOGI(TAG, "NEW_DEV addr=%u", s_usb.dev_addr);
            (void)ensure_midi_ready();
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void usb_midi_host_init(void)
{
    // binary semaphore for TX serialize
    s_usb.tx_done_sem = xSemaphoreCreateBinary();
    if (!s_usb.tx_done_sem) {
        ESP_LOGE(TAG, "tx_done_sem alloc failed");
        return;
    }
    xSemaphoreGive(s_usb.tx_done_sem);

    usb_host_config_t host_cfg = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t e = usb_host_install(&host_cfg);
    if (e == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB Host already installed");
    } else if (e != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(e));
        return; // ✅ no abort → avoid reboot loop
    } else {
        ESP_LOGI(TAG, "USB Host installed");
    }

    xTaskCreatePinnedToCore(usb_host_daemon_task, "usb_daemon", 4096, NULL, 20, NULL, 0);
    xTaskCreatePinnedToCore(usb_client_task, "usb_client", 8192, NULL, 15, NULL, 1);
}
