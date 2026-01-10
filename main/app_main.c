// ===== FILE: main/app_main.c =====
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "portal_wifi.h"
#include "config_store.h"
#include "footswitch.h"
#include "usb_midi_host.h"
#include "uart_midi_out.h"
#include "expfs.h"
#include "display_uart.h"

#include "nvs_flash.h"

static const char *TAG = "APP";

static void bootstrap_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "bootstrap start (reset_reason=%d)", (int)esp_reset_reason());

    // 1) nvs init (ไม่ให้รีบูต)
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (err=%s)", esp_err_to_name(err));
        esp_err_t e2 = nvs_flash_erase();
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(e2));
        }
        err = nvs_flash_init();
    }

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nvs already initialized");
        err = ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s (continue without persistence)", esp_err_to_name(err));
    }

    // 2) config
    ESP_LOGI(TAG, "config_store_init()");
    config_store_init();
    vTaskDelay(pdMS_TO_TICKS(20));

    // 2.1) uart display (bank/switch names)
    ESP_LOGI(TAG, "display_uart_init()");
    display_uart_init();
    vTaskDelay(pdMS_TO_TICKS(20));

    // 3) usb midi host
    ESP_LOGI(TAG, "usb_midi_host_init()");
    usb_midi_host_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3.1) uart midi out
    ESP_LOGI(TAG, "uart_midi_out_init()");
    uart_midi_out_init();
    vTaskDelay(pdMS_TO_TICKS(20));

    // 4) captive portal
    ESP_LOGI(TAG, "portal_wifi_start()");
    portal_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5) footswitch
    ESP_LOGI(TAG, "footswitch_start()");
    footswitch_start();

    // 6) exp/fs
    ESP_LOGI(TAG, "expfs_start()");
    expfs_start();

    ESP_LOGI(TAG, "system ready ✅");

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main enter");

    // แยก init ไป task เพื่อกันค้าง/กัน watchdog ช่วง boot
    xTaskCreatePinnedToCore(bootstrap_task, "bootstrap", 6144, NULL, 8, NULL, 0);

    // ปล่อยให้ task อื่นทำงาน
    vTaskDelete(NULL);
}
