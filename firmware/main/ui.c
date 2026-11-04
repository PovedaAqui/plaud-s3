#include "ui.h"

#include <stdio.h>

#include "avatars.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include <time.h>

static const char *TAG = "ui";

static lv_obj_t *s_status;
static lv_obj_t *s_timer;
static lv_obj_t *s_avatar;
static lv_obj_t *s_hint;
static void (*s_toggle_cb)(void);
static void (*s_stop_cb)(void);
static ui_state_t s_state;
static bool s_suppress_click;

static lv_obj_t *s_main_screen;
static lv_obj_t *s_watch_screen;
static lv_obj_t *s_min_scale;
static lv_obj_t *s_hour_scale;
static lv_obj_t *s_min_line;
static lv_obj_t *s_hour_line;
static lv_obj_t *s_sync_label;
static volatile bool s_watch_active;
static uint32_t s_last_activity;
static int s_last_hour = -1;
static int s_last_min = -1;

static void wake_locked(void);

static void watch_tap(lv_event_t *event)
{
    (void)event;
    wake_locked();
}

static bool time_known(void)
{
    return time(NULL) > 1700000000; /* later than 2023 */
}

static void update_watch_face(void)
{
    static bool logged_sync;
    time_t now = time(NULL);
    bool known = time_known();
    lv_obj_t *sync = s_sync_label;
    if (!known) {
        lv_scale_set_line_needle_value(s_min_scale, s_min_line, 175, 0);
        lv_scale_set_line_needle_value(s_hour_scale, s_hour_line, 115, 0);
        if (sync) {
            lv_obj_remove_flag(sync, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    if (!logged_sync) {
        logged_sync = true;
        ESP_LOGI(TAG, "clock synced %02d:%02d", tm.tm_hour, tm.tm_min);
    }
    if (tm.tm_min != s_last_min) {
        lv_scale_set_line_needle_value(s_min_scale, s_min_line, 175, tm.tm_min);
        s_last_min = tm.tm_min;
    }
    if (tm.tm_hour != s_last_hour) {
        lv_scale_set_line_needle_value(s_hour_scale, s_hour_line, 115, tm.tm_hour % 12);
        s_last_hour = tm.tm_hour;
    }
    if (sync) {
        lv_obj_add_flag(sync, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enter_screensaver(void)
{
    if (s_watch_active) {
        return;
    }
    s_watch_active = true;
    s_last_min = -1;
    s_last_hour = -1;
    update_watch_face();
    lv_screen_load(s_watch_screen);
    bsp_display_brightness_set(CONFIG_PLAUD_DIM_BRIGHTNESS);
    ESP_LOGI(TAG, "screensaver on");
}

static void wake_locked(void)
{
    s_last_activity = lv_tick_get();
    if (!s_watch_active) {
        return;
    }
    s_watch_active = false;
    lv_screen_load(s_main_screen);
    bsp_display_brightness_set(CONFIG_PLAUD_BRIGHTNESS);
    ESP_LOGI(TAG, "screensaver off");
}

void ui_wake(void)
{
    if (!s_watch_active) {
        s_last_activity = lv_tick_get();
        return;
    }
    bsp_display_lock(-1);
    wake_locked();
    bsp_display_unlock();
}

void ui_notify_activity(void)
{
    ui_wake();
}

bool ui_screensaver_active(void)
{
    return s_watch_active;
}

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_watch_active) {
        update_watch_face();
        return;
    }
    if (CONFIG_PLAUD_SCREENSAVER_S > 0 && s_state == UI_READY &&
        lv_tick_elaps(s_last_activity) >= (uint32_t)CONFIG_PLAUD_SCREENSAVER_S * 1000U) {
        enter_screensaver();
    }
}

static void on_avatar_pressed(lv_event_t *event)
{
    (void)event;
    s_suppress_click = false; /* a new gesture, not the tail of a hold */
}

static void on_avatar(lv_event_t *event)
{
    (void)event;
    /* LVGL still emits CLICKED on release after LONG_PRESSED. Swallow exactly
       that click, regardless of how long the hold lasted. */
    if (s_suppress_click) {
        s_suppress_click = false;
        return;
    }
    ESP_LOGI(TAG, "tap");
    if (s_toggle_cb) {
        s_toggle_cb();
    }
}

static void on_avatar_long(lv_event_t *event)
{
    (void)event;
    s_suppress_click = true;
    ESP_LOGI(TAG, "hold");
    if (s_stop_cb) {
        s_stop_cb();
    }
}

void ui_init(void)
{
    bsp_display_lock(-1);
    lv_obj_t *screen = lv_screen_active();
    s_main_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_avatar = lv_image_create(screen);
    lv_image_set_src(s_avatar, &avatar_connecting);
    lv_obj_add_flag(s_avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_avatar, on_avatar_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_avatar, on_avatar, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_avatar, on_avatar_long, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_align(s_avatar, LV_ALIGN_CENTER, 0, -15);

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

    /* Pocket-watch face: black dial, thin ticks, two hands, no ticking seconds. */
    s_watch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_watch_screen, lv_color_black(), 0);
    lv_obj_add_flag(s_watch_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_watch_screen, watch_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ring = lv_obj_create(s_watch_screen);
    lv_obj_set_size(ring, 434, 434);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_black(), 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x2A2F36), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    s_min_scale = lv_scale_create(s_watch_screen);
    lv_obj_set_size(s_min_scale, 430, 430);
    lv_obj_center(s_min_scale);
    lv_scale_set_mode(s_min_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(s_min_scale, 60);
    lv_scale_set_major_tick_every(s_min_scale, 5);
    lv_scale_set_range(s_min_scale, 0, 59);
    lv_scale_set_angle_range(s_min_scale, 360);
    lv_scale_set_rotation(s_min_scale, 270);
    lv_scale_set_label_show(s_min_scale, false);
    lv_obj_set_style_bg_opa(s_min_scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_min_scale, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_min_scale, lv_color_hex(0xD8DEE9), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_min_scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_min_scale, 18, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_min_scale, lv_color_hex(0x5A6470), LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_min_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(s_min_scale, 9, LV_PART_ITEMS);
    lv_obj_remove_flag(s_min_scale, LV_OBJ_FLAG_CLICKABLE);

    s_hour_scale = lv_scale_create(s_watch_screen);
    lv_obj_set_size(s_hour_scale, 430, 430);
    lv_obj_center(s_hour_scale);
    lv_scale_set_mode(s_hour_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(s_hour_scale, 12);
    lv_scale_set_major_tick_every(s_hour_scale, 1);
    lv_scale_set_range(s_hour_scale, 0, 11);
    lv_scale_set_angle_range(s_hour_scale, 360);
    lv_scale_set_rotation(s_hour_scale, 270);
    lv_scale_set_label_show(s_hour_scale, false);
    lv_obj_set_style_bg_opa(s_hour_scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_hour_scale, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_hour_scale, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_hour_scale, 0, LV_PART_ITEMS);
    lv_obj_remove_flag(s_hour_scale, LV_OBJ_FLAG_CLICKABLE);

    s_min_line = lv_line_create(s_min_scale);
    lv_obj_set_style_line_width(s_min_line, 4, 0);
    lv_obj_set_style_line_color(s_min_line, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(s_min_line, true, 0);

    s_hour_line = lv_line_create(s_hour_scale);
    lv_obj_set_style_line_width(s_hour_line, 8, 0);
    lv_obj_set_style_line_color(s_hour_line, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(s_hour_line, true, 0);

    lv_obj_t *hub = lv_obj_create(s_watch_screen);
    lv_obj_set_size(hub, 16, 16);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_white(), 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    s_sync_label = lv_label_create(s_watch_screen);
    lv_label_set_text(s_sync_label, "SYNC");
    lv_obj_set_style_text_color(s_sync_label, lv_color_hex(0xF2C14E), 0);
    lv_obj_set_style_text_font(s_sync_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_sync_label, LV_ALIGN_CENTER, 0, 150);

    s_last_activity = lv_tick_get();
    lv_timer_create(clock_tick, 1000, NULL);

    bsp_display_unlock();
    ui_set_state(UI_CONNECTING);
}

void ui_set_state(ui_state_t state)
{
    s_state = state;
    bsp_display_lock(-1);
    if (state != UI_READY && s_watch_active) {
        wake_locked();
    }
    s_last_activity = lv_tick_get();
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
        lv_label_set_text(s_hint, "TAP PAUSE  HOLD STOP");
        break;
    case UI_PAUSED:
        lv_image_set_src(s_avatar, &avatar_paused);
        lv_label_set_text(s_status, "paused");
        lv_label_set_text(s_hint, "TAP RESUME  HOLD STOP");
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

void ui_set_pending(int count)
{
    bsp_display_lock(-1);
    if (count > 0) {
        lv_label_set_text_fmt(s_status, "ready - %d saved", count);
    } else {
        lv_label_set_text(s_status, "ready");
    }
    bsp_display_unlock();
}

void ui_on_toggle(void (*callback)(void))
{
    s_toggle_cb = callback;
}

void ui_on_stop(void (*callback)(void))
{
    s_stop_cb = callback;
}
