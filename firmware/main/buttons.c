/*
 * Physical buttons.
 *
 * Key1 is a plain GPIO0 button (10K pull-up on the board, 100nF debounce).
 * Key2 is not a GPIO: it pulls the AXP2101 PWRON pin low, so presses are only
 * visible through the PMIC's interrupt-status registers. We poll INTSTS2 and
 * let the PMIC keep ownership of long-press power-off.
 */

#include "buttons.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define KEY1_GPIO GPIO_NUM_0
#define KEY1_DEBOUNCE_SAMPLES 3
#define KEY1_LONG_PRESS_MS 1500

#define AXP2101_ADDR 0x34
#define AXP2101_REG_INTEN2 0x41
#define AXP2101_REG_INTSTS1 0x48
#define AXP2101_REG_INTSTS2 0x49
#define AXP2101_REG_INTSTS3 0x4A
#define AXP2101_INTEN2_PKEY_LONG (1u << 2)
#define AXP2101_INTEN2_PKEY_SHORT (1u << 3)

static i2c_master_dev_handle_t s_axp;
static button_handler_t s_handler;

static esp_err_t axp_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, value, 1, 100);
}

static esp_err_t axp_write(uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(s_axp, buffer, sizeof(buffer), 100);
}

static void axp_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus; power key disabled");
        return;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not on the bus");
        s_axp = NULL;
        return;
    }

    uint8_t enabled = 0;
    if (axp_read(AXP2101_REG_INTEN2, &enabled) == ESP_OK) {
        axp_write(AXP2101_REG_INTEN2, enabled | AXP2101_INTEN2_PKEY_LONG | AXP2101_INTEN2_PKEY_SHORT);
    }

    /* AXP2101 status bits are write-1-to-clear; drop anything latched at boot. */
    axp_write(AXP2101_REG_INTSTS1, 0xFF);
    axp_write(AXP2101_REG_INTSTS2, 0xFF);
    axp_write(AXP2101_REG_INTSTS3, 0xFF);
    ESP_LOGI(TAG, "AXP2101 power key enabled (INTEN2=0x%02x)", enabled);
}

static void emit(button_id_t id, bool long_press)
{
    ESP_LOGI(TAG, "%s %s", id == BUTTON_KEY1 ? "key1" : "pwr", long_press ? "long" : "short");
    if (s_handler) {
        s_handler(id, long_press);
    }
}

static void buttons_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << KEY1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "key1 idle level=%d", gpio_get_level(KEY1_GPIO));

    bool pressed = false;
    int stable = 0;
    TickType_t pressed_at = 0;
    TickType_t next_axp = 0;
    TickType_t last_long = 0;

    for (;;) {
        bool low = gpio_get_level(KEY1_GPIO) == 0;
        if (low == pressed) {
            stable = 0;
        } else if (++stable >= KEY1_DEBOUNCE_SAMPLES) {
            stable = 0;
            pressed = low;
            if (pressed) {
                pressed_at = xTaskGetTickCount();
            } else {
                TickType_t now = xTaskGetTickCount();
                TickType_t held = now - pressed_at;
                if (held > pdMS_TO_TICKS(KEY1_LONG_PRESS_MS)) {
                    last_long = now;
                    emit(BUTTON_KEY1, true);
                } else if (last_long == 0 || now - last_long > pdMS_TO_TICKS(500)) {
                    /* Swallow the short press a bouncing release can produce
                       right after a hold, which would otherwise restart a take. */
                    emit(BUTTON_KEY1, false);
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (s_axp && now >= next_axp) {
            next_axp = now + pdMS_TO_TICKS(100);
            uint8_t status = 0;
            if (axp_read(AXP2101_REG_INTSTS2, &status) == ESP_OK && status) {
                axp_write(AXP2101_REG_INTSTS2, status); /* write 1 to clear */
                if (status & AXP2101_INTEN2_PKEY_LONG) {
                    ESP_LOGW(TAG, "power key long press (PMIC will power off)");
                    emit(BUTTON_POWER, true);
                } else if (status & AXP2101_INTEN2_PKEY_SHORT) {
                    emit(BUTTON_POWER, false);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void buttons_start(button_handler_t handler)
{
    s_handler = handler;
    axp_init();
    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 4, NULL, 0);
}
