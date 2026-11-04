#include "usbcmd.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

static const char *TAG = "usbcmd";

static void usbcmd_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t used = 0;
    uint8_t byte;

    for (;;) {
        int read = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(200));
        if (read != 1) {
            continue;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte != '\n') {
            if (used < sizeof(line) - 1) {
                line[used++] = (char)byte;
            }
            continue;
        }
        line[used] = 0;
        used = 0;

        long long epoch = 0;
        int offset = 0;
        if (sscanf(line, "TIME %lld %d", &epoch, &offset) == 2) {
            ui_set_clock(epoch, offset);
            ESP_LOGI(TAG, "clock set over USB (offset %d)", offset);
            const char ok[] = "TIME OK\n";
            usb_serial_jtag_write_bytes(ok, sizeof(ok) - 1, pdMS_TO_TICKS(100));
        }
    }
}

void usbcmd_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 512;
    cfg.tx_buffer_size = 512;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB serial driver unavailable");
        return;
    }
    xTaskCreate(usbcmd_task, "usbcmd", 3072, NULL, 3, NULL);
}
