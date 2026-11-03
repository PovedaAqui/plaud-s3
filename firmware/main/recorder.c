#include "recorder.h"

#include <errno.h>
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
#include "storage.h"
#include "ui.h"

static const char *TAG = "recorder";

#define SAMPLE_RATE 16000
#define CHUNK_MS 100
#define CHUNK_BYTES (SAMPLE_RATE * CHUNK_MS / 1000 * 2 * CONFIG_PLAUD_CHANNELS)
#define MONO_BYTES (SAMPLE_RATE * CHUNK_MS / 1000 * 2)
#define UPLOAD_CHUNK 4096
#define UPLOAD_RETRY_MS (CONFIG_PLAUD_UPLOAD_RETRY_S * 1000)

#define WS_CONNECTED BIT0
#define WS_STARTED BIT1
#define WS_DONE BIT2

typedef enum { REC_IDLE, REC_RECORDING, REC_PAUSED } rec_state_t;
typedef enum { ACT_NONE, ACT_TOGGLE, ACT_STOP } rec_action_t;

static esp_websocket_client_handle_t s_ws;
static EventGroupHandle_t s_ws_bits;
static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static bool s_spk_open;
static volatile rec_state_t s_state = REC_IDLE;
static volatile rec_action_t s_action;

static FILE *s_file;
static char s_path[64];
static uint32_t s_frames;
static uint32_t s_data_bytes;
static uint32_t s_dropped;

/* ---------------------------------------------------------------- WebSocket */

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

static bool ws_connected(void)
{
    return s_ws && esp_websocket_client_is_connected(s_ws);
}

static bool ws_ensure(void)
{
    if (CONFIG_PLAUD_SERVER_URL[0] == '\0') {
        return false;
    }
    if (ws_connected()) {
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

static bool ws_send_text(const char *text)
{
    if (!ws_connected()) {
        return false;
    }
    return esp_websocket_client_send_text(s_ws, text, strlen(text), pdMS_TO_TICKS(2000)) >= 0;
}

/* ---------------------------------------------------------------- local file */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);                    /* PCM */
    put_le16(h + 22, 1);                    /* mono */
    put_le32(h + 24, SAMPLE_RATE);
    put_le32(h + 28, SAMPLE_RATE * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

static bool file_start(void)
{
    if (storage_next_path(s_path, sizeof(s_path)) != ESP_OK) {
        return false;
    }
    s_file = fopen(s_path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "cannot create %s: errno=%d (%s)", s_path, errno, strerror(errno));
        return false;
    }
    uint8_t zero[44] = {0};
    if (fwrite(zero, 1, sizeof(zero), s_file) != sizeof(zero)) {
        fclose(s_file);
        s_file = NULL;
        return false;
    }
    s_data_bytes = 0;
    return true;
}

static void file_finish(void)
{
    if (!s_file) {
        return;
    }
    uint8_t header[44];
    wav_header(header, s_data_bytes);
    fseek(s_file, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), s_file);
    fclose(s_file);
    s_file = NULL;
    ESP_LOGI(TAG, "saved %s (%lu bytes)", s_path, (unsigned long)s_data_bytes);
}

/* ---------------------------------------------------------------- uploader */

static bool upload_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 44, SEEK_SET);
    long remaining = size - 44;
    if (remaining <= 0) {
        fclose(file);
        remove(path);
        return true;
    }

    if (!ws_ensure()) {
        fclose(file);
        return false;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hello[192];
    snprintf(
        hello, sizeof(hello),
        "{\"type\":\"start\",\"device\":\"%s-%02x%02x%02x\",\"rate\":%d}",
        CONFIG_PLAUD_DEVICE_NAME, mac[3], mac[4], mac[5], SAMPLE_RATE);
    xEventGroupClearBits(s_ws_bits, WS_STARTED | WS_DONE);
    if (!ws_send_text(hello)) {
        fclose(file);
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_ws_bits, WS_STARTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & WS_STARTED)) {
        fclose(file);
        return false;
    }

    static uint8_t buffer[UPLOAD_CHUNK];
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            break;
        }
        if (esp_websocket_client_send_bin(
                s_ws, (const char *)buffer, got, pdMS_TO_TICKS(5000)) < 0) {
            fclose(file);
            return false;
        }
        remaining -= got;
    }
    fclose(file);

    if (!ws_send_text("{\"type\":\"stop\"}")) {
        return false;
    }
    bits = xEventGroupWaitBits(s_ws_bits, WS_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WS_DONE)) {
        return false;
    }
    remove(path);
    ESP_LOGI(TAG, "uploaded %s", path);
    return true;
}

static void upload_pending_once(void)
{
    char path[64];
    if (!storage_first_pending(path, sizeof(path))) {
        return;
    }
    ui_set_state(UI_SAVING);
    ui_set_status("uploading");
    bool ok = upload_file(path);
    ui_set_state(UI_READY);
    if (!ok) {
        ESP_LOGW(TAG, "upload of %s postponed", path);
    }
    ui_set_pending(storage_pending());
}

/* ---------------------------------------------------------------- audio I/O */

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

/* ---------------------------------------------------------------- state machine */

static void begin_recording(void)
{
    if (!file_start()) {
        ui_set_state(UI_ERROR);
        ui_set_status("storage unavailable");
        return;
    }
    if (!mic_open()) {
        file_finish();
        remove(s_path);
        ui_set_state(UI_ERROR);
        ui_set_status("mic unavailable");
        return;
    }
    s_frames = 0;
    s_dropped = 0;
    ui_set_timer(0);
    ui_set_state(UI_RECORDING);
    if (!ws_connected()) {
        ui_set_status("recording - offline");
    }
    s_state = REC_RECORDING;
}

static void finish_recording(void)
{
    file_finish();
    mic_close();
    s_state = REC_IDLE;
    /* Drop gestures that arrived while the hold was still being released or the
       file was uploading; a stop must not roll straight into a new recording. */
    s_action = ACT_NONE;
    ui_set_timer(0);

    if (ws_connected()) {
        upload_pending_once();
        s_action = ACT_NONE; /* taps during the upload should not start a take */
    } else {
        ui_set_state(UI_READY);
        ui_set_pending(storage_pending());
    }
}

static void recorder_task(void *arg)
{
    (void)arg;
    static int16_t in_buf[CHUNK_BYTES / 2];
    static int16_t out_buf[MONO_BYTES / 2];
    static TickType_t next_retry;

    for (;;) {
        rec_action_t action = s_action;
        if (action != ACT_NONE) {
            s_action = ACT_NONE;
        }

        switch (s_state) {
        case REC_IDLE:
            if (action == ACT_TOGGLE) {
                begin_recording();
            } else if (action == ACT_STOP) {
                /* nothing to stop */
            } else if (xTaskGetTickCount() >= next_retry) {
                next_retry = xTaskGetTickCount() + pdMS_TO_TICKS(UPLOAD_RETRY_MS);
                if (storage_pending() > 0 && ws_connected()) {
                    upload_pending_once();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case REC_RECORDING:
            if (action == ACT_STOP) {
                finish_recording();
                break;
            }
            if (action == ACT_TOGGLE) {
                mic_close();
                ui_set_state(UI_PAUSED);
                s_state = REC_PAUSED;
                break;
            }
            {
                int ret = esp_codec_dev_read(s_mic, in_buf, sizeof(in_buf));
                if (ret != ESP_CODEC_DEV_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }
                const int samples = sizeof(in_buf) / 2;
                int out_samples = samples;
                if (CONFIG_PLAUD_CHANNELS == 2) {
                    out_samples = samples / 2;
                    for (int i = 0; i < out_samples; i++) {
                        int32_t mix = (int32_t)in_buf[2 * i] + (int32_t)in_buf[2 * i + 1];
                        out_buf[i] = (int16_t)(mix / 2);
                    }
                } else {
                    memcpy(out_buf, in_buf, sizeof(in_buf));
                }
                size_t bytes = (size_t)out_samples * 2;
                if (s_file && fwrite(out_buf, 1, bytes, s_file) == bytes) {
                    s_data_bytes += bytes;
                    s_frames += out_samples;
                    ui_set_timer(s_frames / SAMPLE_RATE);
                } else {
                    ESP_LOGE(TAG, "write failed, stopping");
                    ui_set_state(UI_ERROR);
                    ui_set_status("storage full");
                    finish_recording();
                }
            }
            break;

        case REC_PAUSED:
            if (action == ACT_STOP) {
                finish_recording();
                break;
            }
            if (action == ACT_TOGGLE) {
                if (mic_open()) {
                    ui_set_state(UI_RECORDING);
                    if (!ws_connected()) {
                        ui_set_status("recording - offline");
                    }
                    s_state = REC_RECORDING;
                } else {
                    ui_set_state(UI_ERROR);
                    ui_set_status("mic unavailable");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
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
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (int n = 0; n < (int)(sizeof(buf) / 2); n++) {
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
        ESP_LOGI(TAG, "selftest: %d samples, rms=%d, peak=%d", samples,
                 (int)sqrt((double)(sum / samples)), peak);
    } else {
        ESP_LOGE(TAG, "selftest: no audio");
    }
}

void recorder_init(void)
{
    s_ws_bits = xEventGroupCreate();
    if (storage_init() != ESP_OK) {
        ui_set_state(UI_ERROR);
        ui_set_status("storage unavailable");
    }
    ui_set_state(UI_CONNECTING);
    bool ok = ws_ensure();
    ui_set_state(UI_READY);
    ui_set_pending(storage_pending());
    if (!ok) {
        ui_set_status(
            CONFIG_PLAUD_SERVER_URL[0] == '\0' ? "offline - no server url" : "offline - saves locally");
    }
}

void recorder_start(void)
{
    xTaskCreatePinnedToCore(recorder_task, "recorder", 8192, NULL, 5, NULL, 1);
}

void recorder_toggle(void)
{
    s_action = ACT_TOGGLE;
}

void recorder_stop(void)
{
    if (s_state != REC_IDLE) {
        s_action = ACT_STOP;
    }
}

bool recorder_active(void)
{
    return s_state != REC_IDLE;
}
