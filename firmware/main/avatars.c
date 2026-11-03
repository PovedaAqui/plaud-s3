/*
 * DiceBear avatars as linked binary blobs (see scripts/gen_avatars.py).
 * The .bin files are raw RGB565, 240x240, flattened onto black.
 */

#include "avatars.h"

#include <stdint.h>

#define AVATAR_SIZE 240
#define AVATAR_BYTES (AVATAR_SIZE * AVATAR_SIZE * 2)

#define AVATAR_DSC(symbol)                                                 \
    {                                                                     \
        .header = {                                                       \
            .magic = LV_IMAGE_HEADER_MAGIC,                               \
            .cf = LV_COLOR_FORMAT_RGB565,                                 \
            .w = AVATAR_SIZE,                                             \
            .h = AVATAR_SIZE,                                             \
            .stride = AVATAR_SIZE * 2,                                    \
        },                                                                \
        .data_size = AVATAR_BYTES,                                        \
        .data = symbol,                                                   \
    }

extern const uint8_t _binary_avatar_connecting_bin_start[]
    asm("_binary_avatar_connecting_bin_start");
extern const uint8_t _binary_avatar_ready_bin_start[]
    asm("_binary_avatar_ready_bin_start");
extern const uint8_t _binary_avatar_recording_bin_start[]
    asm("_binary_avatar_recording_bin_start");
extern const uint8_t _binary_avatar_paused_bin_start[]
    asm("_binary_avatar_paused_bin_start");
extern const uint8_t _binary_avatar_saving_bin_start[]
    asm("_binary_avatar_saving_bin_start");
extern const uint8_t _binary_avatar_error_bin_start[]
    asm("_binary_avatar_error_bin_start");

const lv_image_dsc_t avatar_connecting = AVATAR_DSC(_binary_avatar_connecting_bin_start);
const lv_image_dsc_t avatar_ready = AVATAR_DSC(_binary_avatar_ready_bin_start);
const lv_image_dsc_t avatar_recording = AVATAR_DSC(_binary_avatar_recording_bin_start);
const lv_image_dsc_t avatar_paused = AVATAR_DSC(_binary_avatar_paused_bin_start);
const lv_image_dsc_t avatar_saving = AVATAR_DSC(_binary_avatar_saving_bin_start);
const lv_image_dsc_t avatar_error = AVATAR_DSC(_binary_avatar_error_bin_start);
