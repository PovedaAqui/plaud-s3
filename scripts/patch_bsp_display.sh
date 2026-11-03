#!/usr/bin/env bash
# Two display-driver adjustments for this panel, re-applied after the component
# manager re-fetches the Waveshare BSP:
#
# 1. 20-row render bands. The default 50-row bands (466x50x2 = 46 KB) make the
#    QSPI path return ESP_ERR_NO_MEM when a large widget (a 180 px avatar)
#    repaints; 20-row bands stay inside the DMA budget.
# 2. max_transfer_sz capped at 50 rows so the SPI driver never reserves a
#    full-screen DMA buffer.
set -euo pipefail
file="$(dirname "$0")/../firmware/managed_components/waveshare__esp32_s3_touch_amoled_1_75c/esp32_s3_touch_amoled_1_75c.c"
[[ -f "$file" ]] || { echo "BSP not fetched yet: $file" >&2; exit 1; }
sed -i 's/\.buffer_height = 50,/.buffer_height = 20,/' "$file"
sed -i 's/\.max_transfer_sz = BSP_LCD_H_RES \* BSP_LCD_V_RES \* BSP_LCD_BITS_PER_PIXEL \/ 8,/.max_transfer_sz = BSP_LCD_H_RES * 50 * BSP_LCD_BITS_PER_PIXEL \/ 8,/' "$file"
grep -n "buffer_height\|max_transfer_sz" "$file" | head -3
