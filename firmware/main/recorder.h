#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void recorder_init(void);
void recorder_start(void);

/* Opens the mic for one second and logs the signal level. */
void recorder_selftest(void);

/* Called from the UI task: toggles between idle and recording. */
void recorder_toggle(void);
void recorder_stop(void);
bool recorder_active(void);

#ifdef __cplusplus
}
#endif
