# ===== FILE: main/CMakeLists.txt =====
idf_component_register(
    SRCS
        "app_main.c"
        "portal_wifi.c"
        "dns_hijack.c"
        "config_store.c"
        "footswitch.c"
        "midi_actions.c"
        "usb_midi_host.c"
        "uart_midi_out.c"
        "expfs.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        spiffs
        driver esp_adc
        lwip
        json
        usb
        esp_timer
)
