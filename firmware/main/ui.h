#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_CONNECTING,
    UI_READY,
    UI_RECORDING,
    UI_PAUSED,
    UI_SAVING,
    UI_ERROR,
} ui_state_t;

void ui_init(void);
void ui_set_state(ui_state_t state);
void ui_set_status(const char *text);
void ui_set_timer(uint32_t seconds);
void ui_set_pending(int count);
void ui_on_toggle(void (*callback)(void));
void ui_on_stop(void (*callback)(void));

/* Pocket-watch screensaver: first tap only wakes the device. */
bool ui_screensaver_active(void);
void ui_wake(void);
void ui_notify_activity(void);

#ifdef __cplusplus
}
#endif
