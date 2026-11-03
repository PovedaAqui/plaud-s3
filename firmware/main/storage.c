#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "storage";
static const char *MOUNT = "/storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t storage_init(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mounted %s (%d pending)", MOUNT, storage_pending());

    /* Prove the volume is writable before the recorder relies on it. */
    char probe[32];
    snprintf(probe, sizeof(probe), "%s/PROBE.TXT", MOUNT);
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

static bool is_wav(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".wav") == 0;
}

int storage_pending(void)
{
    DIR *dir = opendir(MOUNT);
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

esp_err_t storage_next_path(char *out, size_t len)
{
    DIR *dir = opendir(MOUNT);
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
    snprintf(out, len, "%s/rec_%06d.wav", MOUNT, highest + 1);
    return ESP_OK;
}

bool storage_first_pending(char *out, size_t len)
{
    DIR *dir = opendir(MOUNT);
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
    snprintf(out, len, "%s/%s", MOUNT, best);
    return true;
}
