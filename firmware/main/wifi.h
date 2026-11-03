#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_start(void);

/* Blocks until the station gets an IP or the timeout expires. */
bool wifi_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
