#include "recorder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ui.h"

static const char *TAG = "recorder";

#define SAMPLE_RATE 16000
#define CHUNK_MS 100
#define CHUNK_BYTES (SAMPLE_RATE * CHUNK_MS / 1000 * 2 * CONFIG_PLAUD_CHANNELS)
#define MONO_BYTES (SAMPLE_RATE * CHUNK_MS / 1000 * 2)

#define WS_CONNECTED BIT0
#define WS_STARTED BIT1
#define WS_DONE BIT2

typedef enum { REC_IDLE, REC_ARMING, REC_RECORDING, REC_STOPPING } rec_state_t;

static esp_websocket_client_handle_t s_ws;
static EventGroupHandle_t s_ws_bits;
static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static bool s_spk_open;
static volatile rec_state_t s_state = REC_IDLE;
static volatile bool s_toggle_request;
static uint32_t s_dropped;

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "server connected");
        xEventGroupSetBits(s_ws_bits, WS_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "server disconnected");
        xEventGroupClearBits(s_ws_bits, WS_CONNECTED | WS_STARTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (event->op_code == 0x01 && event->data_ptr && event->data_len > 0) {
            if (strstr(event->data_ptr, "\"started\"")) {
                xEventGroupSetBits(s_ws_bits, WS_STARTED);
            } else if (strstr(event->data_ptr, "\"done\"")) {
                xEventGroupSetBits(s_ws_bits, WS_DONE);
            } else if (strstr(event->data_ptr, "\"error\"")) {
                ESP_LOGW(TAG, "server error: %.*s", event->data_len, event->data_ptr);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        break;
    default:
        break;
    }
}

static bool ws_send_text(const char *text)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        return false;
    }
    return esp_websocket_client_send_text(s_ws, text, strlen(text), pdMS_TO_TICKS(2000)) >= 0;
}

/* Keep one WebSocket open across recordings so a tap starts capture immediately. */
static bool ws_ensure(void)
{
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        return true;
    }
    if (!s_ws) {
        esp_websocket_client_config_t cfg = {
            .uri = CONFIG_PLAUD_SERVER_URL,
            .reconnect_timeout_ms = 2000,
            .network_timeout_ms = 5000,
            .buffer_size = 8192,
            .task_stack = 6144,
            .disable_auto_reconnect = false,
        };
        s_ws = esp_websocket_client_init(&cfg);
        if (!s_ws) {
            return false;
        }
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        if (esp_websocket_client_start(s_ws) != ESP_OK) {
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
            return false;
        }
    } else {
        esp_websocket_client_start(s_ws);
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_ws_bits, WS_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    return (bits & WS_CONNECTED) != 0;
}

static bool begin_session(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hello[192];
    snprintf(
        hello, sizeof(hello),
        "{\"type\":\"start\",\"device\":\"%s-%02x%02x%02x\",\"rate\":%d}",
        CONFIG_PLAUD_DEVICE_NAME, mac[3], mac[4], mac[5], SAMPLE_RATE);
    xEventGroupClearBits(s_ws_bits, WS_STARTED | WS_DONE);
    if (!ws_send_text(hello)) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_ws_bits, WS_STARTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    return (bits & WS_STARTED) != 0;
}

static bool mic_open(void)
{
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "microphone init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = CONFIG_PLAUD_CHANNELS,
        .sample_rate = SAMPLE_RATE,
    };
    if (s_spk && !s_spk_open) {
        if (esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK) {
            s_spk_open = true;
        } else {
            ESP_LOGW(TAG, "speaker codec open failed");
        }
    }
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }
    return true;
}

static void mic_close(void)
{
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
}

static void recorder_task(void *arg)
{
    (void)arg;
    static int16_t in_buf[CHUNK_BYTES / 2];
    static int16_t out_buf[MONO_BYTES / 2];
    uint32_t frames = 0;

    for (;;) {
        if (s_state == REC_IDLE) {
            if (s_toggle_request) {
                s_toggle_request = false;
                ui_set_state(UI_CONNECTING);
                if (!mic_open()) {
                    ui_set_state(UI_ERROR);
                    ui_set_status("mic unavailable");
                } else if (!ws_ensure() || !begin_session()) {
                    ui_set_state(UI_ERROR);
                    ui_set_status("no server");
                    mic_close();
                } else {
                    s_state = REC_RECORDING;
                    frames = 0;
                    ui_set_timer(0);
                    ui_set_state(UI_RECORDING);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_state == REC_RECORDING) {
            if (s_toggle_request) { /* stop requested */
                s_toggle_request = false;
                s_state = REC_STOPPING;
                continue;
            }

            int ret = esp_codec_dev_read(s_mic, in_buf, sizeof(in_buf));
            if (ret != ESP_CODEC_DEV_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const int bytes = sizeof(in_buf);

            const int samples = bytes / 2;
            int out_samples;
            if (CONFIG_PLAUD_CHANNELS == 2) {
                out_samples = samples / 2;
                for (int i = 0; i < out_samples; i++) {
                    int32_t mix = (int32_t)in_buf[2 * i] + (int32_t)in_buf[2 * i + 1];
                    out_buf[i] = (int16_t)(mix / 2);
                }
            } else {
                out_samples = samples;
                memcpy(out_buf, in_buf, bytes);
            }

            if (esp_websocket_client_is_connected(s_ws)) {
                int sent = -1;
                for (int attempt = 0; attempt < 3 && sent < 0; attempt++) {
                    sent = esp_websocket_client_send_bin(
                        s_ws, (const char *)out_buf, out_samples * 2, pdMS_TO_TICKS(2000));
                    if (sent < 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
                if (sent < 0) {
                    s_dropped += out_samples;
                }
            } else {
                s_dropped += out_samples;
            }
            frames += out_samples;
            ui_set_timer(frames / SAMPLE_RATE);
            continue;
        }

        if (s_state == REC_STOPPING) {
            ui_set_state(UI_SAVING);
            bool sent = ws_send_text("{\"type\":\"stop\"}");
            EventBits_t done = xEventGroupWaitBits(
                s_ws_bits, WS_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            ESP_LOGI(TAG, "stop sent=%d done=%d frames=%lu dropped=%lu",
                     sent, (done & WS_DONE) != 0, (unsigned long)frames, (unsigned long)s_dropped);
            mic_close();
            ui_set_state(UI_READY);
            s_state = REC_IDLE;
        }
    }
}

void recorder_selftest(void)
{
    if (!mic_open()) {
        ESP_LOGE(TAG, "selftest: mic open failed");
        return;
    }
    esp_codec_dev_set_in_gain(s_mic, 24.0f);
    static int16_t buf[CHUNK_BYTES / 2];
    int64_t sum = 0;
    int peak = 0;
    int samples = 0;
    for (int i = 0; i < 10; i++) {
        int ret = esp_codec_dev_read(s_mic, buf, sizeof(buf));
        if (i == 0) {
            ESP_LOGI(TAG, "selftest: first read returned %d", ret);
        }
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const int bytes = sizeof(buf);
        for (int n = 0; n < bytes / 2; n++) {
            int v = buf[n];
            sum += (int64_t)v * v;
            if (abs(v) > peak) {
                peak = abs(v);
            }
            samples++;
        }
    }
    mic_close();
    if (samples) {
        ESP_LOGI(TAG, "selftest: %d samples, rms=%d, peak=%d", samples, (int)sqrt((double)(sum / samples)), peak);
    } else {
        ESP_LOGE(TAG, "selftest: no audio");
    }
}

void recorder_init(void)
{
    s_ws_bits = xEventGroupCreate();
    ui_set_state(UI_CONNECTING);
    bool ok = ws_ensure();
    ui_set_state(ok ? UI_READY : UI_ERROR);
    if (!ok) {
        ui_set_status("no server");
    }
}

void recorder_start(void)
{
    xTaskCreatePinnedToCore(recorder_task, "recorder", 8192, NULL, 5, NULL, 1);
}

void recorder_toggle(void)
{
    s_toggle_request = true;
}
