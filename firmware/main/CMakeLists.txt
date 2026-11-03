idf_component_register(
    SRCS "main.c" "wifi.c" "recorder.c" "ui.c" "avatars.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_timer
)
