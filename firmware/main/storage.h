#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the `storage` partition (FAT, wear-levelled) at /storage, formatting
 * it on first use. */
esp_err_t storage_init(void);

/* Number of .wav files waiting to be uploaded. */
int storage_pending(void);

/* Next free path (e.g. /storage/rec_000007.wav), ordered by existing names. */
esp_err_t storage_next_path(char *out, size_t len);

/* Oldest pending file, or false when the queue is empty. */
bool storage_first_pending(char *out, size_t len);

/* Active volume for new recordings ("/storage" or "/sdcard"). */
const char *storage_root(void);
bool storage_is_external(void);

#ifdef __cplusplus
}
#endif
