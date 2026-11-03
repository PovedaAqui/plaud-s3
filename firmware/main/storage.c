#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#if CONFIG_PLAUD_SD_ENABLE
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "storage";
static const char *INTERNAL_ROOT = "/storage";
static const char *EXTERNAL_ROOT = "/sdcard";
static const char *s_root = "/storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_sd_mounted;

static bool is_wav(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".wav") == 0;
}

static int pending_in(const char *root)
{
    DIR *dir = opendir(root);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_wav(entry->d_name)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static bool first_in(const char *root, char *out, size_t len)
{
    DIR *dir = opendir(root);
    if (!dir) {
        return false;
    }
    char best[64] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_wav(entry->d_name) && (best[0] == '\0' || strcmp(entry->d_name, best) < 0)) {
            strlcpy(best, entry->d_name, sizeof(best));
        }
    }
    closedir(dir);
    if (best[0] == '\0') {
        return false;
    }
    snprintf(out, len, "%s/%s", root, best);
    return true;
}

static void mount_external(void)
{
#if CONFIG_PLAUD_SD_ENABLE
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_PLAUD_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = CONFIG_PLAUD_SD_CLK;
    slot.cmd = CONFIG_PLAUD_SD_CMD;
    slot.d0 = CONFIG_PLAUD_SD_D0;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(EXTERNAL_ROOT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no microSD card (%s); using internal flash", esp_err_to_name(err));
        return;
    }

    s_sd_mounted = true;
    s_root = EXTERNAL_ROOT;
    ESP_LOGI(TAG, "microSD mounted at %s (%llu MB)", EXTERNAL_ROOT,
             ((unsigned long long)card->csd.capacity * card->csd.sector_size) / (1024 * 1024));
#else
    ESP_LOGI(TAG, "microSD support disabled in menuconfig");
#endif
}

esp_err_t storage_init(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(INTERNAL_ROOT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_root = INTERNAL_ROOT;
    s_sd_mounted = false;
    mount_external();

    ESP_LOGI(TAG, "recording to %s (%d pending, internal=%d)", s_root, storage_pending(),
             pending_in(INTERNAL_ROOT));

    /* Prove the active volume is writable before the recorder relies on it. */
    char probe[48];
    snprintf(probe, sizeof(probe), "%s/PROBE.TXT", s_root);
    FILE *f = fopen(probe, "wb");
    if (f) {
        fputc('x', f);
        fclose(f);
        remove(probe);
        ESP_LOGI(TAG, "write test ok");
    } else {
        ESP_LOGE(TAG, "write test failed: errno=%d (%s)", errno, strerror(errno));
    }
    return ESP_OK;
}

int storage_pending(void)
{
    int count = pending_in(INTERNAL_ROOT);
    if (s_sd_mounted) {
        count += pending_in(EXTERNAL_ROOT);
    }
    return count;
}

esp_err_t storage_next_path(char *out, size_t len)
{
    DIR *dir = opendir(s_root);
    if (!dir) {
        return ESP_FAIL;
    }
    int highest = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int index = 0;
        if (sscanf(entry->d_name, "rec_%d.wav", &index) == 1 && index > highest) {
            highest = index;
        }
    }
    closedir(dir);
    snprintf(out, len, "%s/rec_%06d.wav", s_root, highest + 1);
    return ESP_OK;
}

bool storage_first_pending(char *out, size_t len)
{
    /* Drain the internal partition first: those files predate a newly inserted card. */
    if (first_in(INTERNAL_ROOT, out, len)) {
        return true;
    }
    return s_sd_mounted && first_in(EXTERNAL_ROOT, out, len);
}

const char *storage_root(void)
{
    return s_root;
}

bool storage_is_external(void)
{
    return s_sd_mounted;
}
