#include "bsp/esp-bsp.h"
#include "buttons.h"
#include "esp_log.h"
#include "esp_sntp.h"
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

    /* A press while the watch is showing only wakes the device. */
    if (ui_screensaver_active()) {
        ui_wake();
        return;
    }

    if (id == BUTTON_KEY1) {
        /* Tap cycles record -> pause -> resume; hold stops and saves. */
        if (long_press) {
            recorder_stop();
        } else {
            recorder_toggle();
        }
        return;
    }

    /* Power key: stop when something is running, otherwise start. */
    if (recorder_active()) {
        recorder_stop();
    } else {
        recorder_toggle();
    }
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
    bsp_display_brightness_set(CONFIG_PLAUD_BRIGHTNESS);
    ui_init();
    ui_set_state(UI_CONNECTING);
    ui_set_status("joining wi-fi");
    ui_on_toggle(recorder_toggle);
    ui_on_stop(recorder_stop);

    recorder_selftest();
    wifi_start();
    bool online = wifi_wait_connected(30000);
    if (online) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, CONFIG_PLAUD_NTP_SERVER);
        esp_sntp_init();
    }
    if (!online) {
        ESP_LOGE(TAG, "no Wi-Fi; check menuconfig credentials");
        ui_set_state(UI_ERROR);
        ui_set_status("no wi-fi - check config");
    }

    recorder_init();
    recorder_start();
    buttons_start(on_button);

#if CONFIG_PLAUD_AUTOTEST_SECONDS > 0
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "autotest: record/pause/resume/stop");
    recorder_toggle();                                            /* start */
    vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAUD_AUTOTEST_SECONDS * 300));
    recorder_toggle();                                            /* pause */
    vTaskDelay(pdMS_TO_TICKS(1200));
    recorder_toggle();                                            /* resume */
    vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAUD_AUTOTEST_SECONDS * 300));
    recorder_stop();                                              /* stop */
#endif

    ESP_LOGI(TAG, "ready");
}
