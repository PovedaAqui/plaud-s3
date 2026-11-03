idf_component_register(
    SRCS "main.c" "wifi.c" "recorder.c" "ui.c" "avatars.c" "buttons.c" "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_timer driver fatfs wear_levelling sdmmc
)

set(AVATAR_BINS
    assets/avatar_connecting.bin
    assets/avatar_ready.bin
    assets/avatar_recording.bin
    assets/avatar_paused.bin
    assets/avatar_saving.bin
    assets/avatar_error.bin)
foreach(bin ${AVATAR_BINS})
    target_add_binary_data(${COMPONENT_LIB} ${bin} BINARY)
endforeach()
