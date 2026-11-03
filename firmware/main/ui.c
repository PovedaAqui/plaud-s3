#include "ui.h"

#include <stdio.h>

#include "avatars.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui";

static lv_obj_t *s_status;
static lv_obj_t *s_timer;
static lv_obj_t *s_avatar;
static lv_obj_t *s_hint;
static void (*s_toggle_cb)(void);
static ui_state_t s_state;

static void on_avatar(lv_event_t *event)
{
    (void)event;
    ESP_LOGI(TAG, "tap");
    if (s_toggle_cb) {
        s_toggle_cb();
    }
}

void ui_init(void)
{
    bsp_display_lock(-1);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(screen);
    lv_label_set_text(brand, "PLAUD-S3");
    lv_obj_set_style_text_color(brand, lv_color_hex(0x8A94A6), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_20, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 40);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "starting");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xF2C14E), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status, 300);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *badge = lv_obj_create(screen);
    lv_obj_set_size(badge, 200, 200);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0xEFE7DA), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(badge, on_avatar, LV_EVENT_CLICKED, NULL);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -15);

    s_avatar = lv_image_create(badge);
    lv_image_set_src(s_avatar, &avatar_connecting);
    lv_obj_center(s_avatar);

    s_timer = lv_label_create(screen);
    lv_label_set_text(s_timer, "00:00");
    lv_obj_set_style_text_color(s_timer, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_timer, &lv_font_montserrat_48, 0);
    lv_obj_align(s_timer, LV_ALIGN_CENTER, 0, 120);

    s_hint = lv_label_create(screen);
    lv_label_set_text(s_hint, "TAP TO RECORD");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6B7684), 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 165);

    bsp_display_unlock();
    ui_set_state(UI_CONNECTING);
}

void ui_set_state(ui_state_t state)
{
    s_state = state;
    bsp_display_lock(-1);
    switch (state) {
    case UI_CONNECTING:
        lv_image_set_src(s_avatar, &avatar_connecting);
        lv_label_set_text(s_status, "connecting");
        lv_label_set_text(s_hint, "WAIT");
        break;
    case UI_READY:
        lv_image_set_src(s_avatar, &avatar_ready);
        lv_label_set_text(s_status, "ready");
        lv_label_set_text(s_hint, "TAP TO RECORD");
        break;
    case UI_RECORDING:
        lv_image_set_src(s_avatar, &avatar_recording);
        lv_label_set_text(s_status, "recording");
        lv_label_set_text(s_hint, "TAP TO STOP");
        break;
    case UI_SAVING:
        lv_image_set_src(s_avatar, &avatar_saving);
        lv_label_set_text(s_status, "saving");
        lv_label_set_text(s_hint, "WAIT");
        break;
    case UI_ERROR:
        lv_image_set_src(s_avatar, &avatar_error);
        lv_label_set_text(s_status, "offline");
        lv_label_set_text(s_hint, "TAP TO RETRY");
        break;
    }
    bsp_display_unlock();
}

void ui_set_status(const char *text)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_status, text);
    bsp_display_unlock();
}

void ui_set_timer(uint32_t seconds)
{
    static uint32_t last;
    if (seconds == last) {
        return; /* 1 Hz is all the screen needs */
    }
    last = seconds;
    bsp_display_lock(-1);
    lv_label_set_text_fmt(s_timer, "%02u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    bsp_display_unlock();
}

void ui_on_toggle(void (*callback)(void))
{
    s_toggle_cb = callback;
}
