#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "buttons.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "recorder.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "plaud-s3";

static void on_button(button_id_t id, bool long_press)
{
    if (id == BUTTON_POWER && long_press) {
        return; /* the PMIC owns long-press power-off */
    }
    ESP_LOGI(TAG, "button %s", long_press ? "long" : "short");
    recorder_toggle();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_display_start();
    bsp_display_brightness_set(80);
    ui_init();
    ui_set_state(UI_CONNECTING);
    ui_set_status("joining wi-fi");
    ui_on_toggle(recorder_toggle);

    recorder_selftest();
    wifi_start();
    if (!wifi_wait_connected(30000)) {
        ESP_LOGE(TAG, "no Wi-Fi; check menuconfig credentials");
        ui_set_state(UI_ERROR);
        ui_set_status("no wi-fi - check config");
    }

    recorder_init();
    recorder_start();
    buttons_start(on_button);

#if CONFIG_PLAUD_AUTOTEST_SECONDS > 0
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "autotest: recording for %d s", CONFIG_PLAUD_AUTOTEST_SECONDS);
    recorder_toggle();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAUD_AUTOTEST_SECONDS * 1000));
    recorder_toggle();
#endif

    ESP_LOGI(TAG, "ready");
}
