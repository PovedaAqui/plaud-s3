#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Listens on the USB Serial/JTAG port for host commands such as
 * `TIME <epoch> <utc_offset_seconds>` so a laptop can set the clock over USB. */
void usbcmd_start(void);

#ifdef __cplusplus
}
#endif
