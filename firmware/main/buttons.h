#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_KEY1,   /* GPIO0, the BOOT key on the left side */
    BUTTON_POWER,  /* Key2, wired to the AXP2101 PWRON input */
} button_id_t;

typedef void (*button_handler_t)(button_id_t id, bool long_press);

/* Starts the 20 ms polling task (GPIO0 + AXP2101 PWRON IRQ). */
void buttons_start(button_handler_t handler);

#ifdef __cplusplus
}
#endif
