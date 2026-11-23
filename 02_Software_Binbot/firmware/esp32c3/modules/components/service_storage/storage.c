#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"

static const char *TAG = "service_storage";
static bool s_mounted;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_storage_mutex;

struct storage_file {
    FILE *file;
};

#define STORAGE_UPDATE_BUFFER_SIZE 4096
#define STORAGE_MAX_FILE_SIZE (512 * 1024)

static uint8_t s_update_buffer[STORAGE_UPDATE_BUFFER_SIZE];
/* Match FAT/WL's 4 KiB sector so small UART packets do not repeatedly rewrite
 * a Flash sector. This bounded cache also works on the C3 without PSRAM. */
static uint8_t s_gif_update_buffer[STORAGE_UPDATE_BUFFER_SIZE];
static size_t s_gif_update_buffered;
static int s_gif_update_fd = -1;
static char s_gif_update_name[64];
static char s_gif_update_temp[sizeof(BINBOT_STORAGE_PATH) + 70];
static size_t s_gif_update_expected;
static size_t s_gif_update_written;
static uint8_t s_gif_update_sha256[32];
static mbedtls_md_context_t s_gif_update_md;
static bool s_gif_update_md_ready;
static storage_gif_update_callback_t s_gif_update_callback;
static void *s_gif_update_callback_context;

static bool is_safe_file_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > 63 ||
        name[0] == '.' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == ':' ||
            *cursor == ' ' || *cursor == '\t') {
            return false;
        }
    }
    return true;
}

static void remove_legacy_display_assets(void);

esp_err_t storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (s_storage_mutex == NULL) {
        s_storage_mutex = xSemaphoreCreateMutex();
        if (s_storage_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t error = esp_vfs_fat_spiflash_mount_rw_wl(
        BINBOT_STORAGE_PATH, "storage", &mount_config, &s_wl_handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mount storage failed: %s", esp_err_to_name(error));
        return error;
    }
    s_mounted = true;
    remove_legacy_display_assets();
    ESP_LOGI(TAG, "FAT filesystem mounted at %s", BINBOT_STORAGE_PATH);
    return ESP_OK;
}

static esp_err_t make_file_path(const char *name, char *path, size_t path_size)
{
    if (!is_safe_file_name(name) || path == NULL || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(path, path_size, "%s/%s",
                                 BINBOT_STORAGE_PATH, name);
    return written > 0 && (size_t)written < path_size ? ESP_OK :
           ESP_ERR_INVALID_SIZE;
}

static void remove_legacy_display_assets(void)
{
    /* Older storage images shipped these files, but the runtime slot is now
     * always display.gif. Remove them on boot so a replacement has room for
     * its temporary file and the existing asset remains atomic. */
    static const char *const legacy_names[] = {
        "lulu.gif",
        "labi.gif",
        "miao.gif",
        "miao.rgb565",
        "figma_face.gif",
    };
    for (size_t i = 0; i < sizeof(legacy_names) / sizeof(legacy_names[0]); ++i) {
        char path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
        if (make_file_path(legacy_names[i], path, sizeof(path)) == ESP_OK &&
            remove(path) == 0) {
            ESP_LOGI(TAG, "removed legacy display asset: %s", legacy_names[i]);
        }
    }
}

esp_err_t storage_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    if (s_storage_mutex == NULL ||
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = esp_vfs_fat_spiflash_unmount_rw_wl(
        BINBOT_STORAGE_PATH, s_wl_handle);
    if (error != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return error;
    }
    s_mounted = false;
    s_wl_handle = WL_INVALID_HANDLE;
    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

esp_err_t storage_open_file(const char *path, const char *mode, FILE **file)
{
    if (path == NULL || mode == NULL || file == NULL ||
        strncmp(path, BINBOT_STORAGE_PATH "/", sizeof(BINBOT_STORAGE_PATH)) != 0 ||
        strcmp(mode, "rb") != 0 || !s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *file = NULL;
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    FILE *opened = fopen(path, mode);
    if (opened == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *file = opened;
    return ESP_OK;
}

esp_err_t storage_close_file(FILE *file)
{
    if (file == NULL || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int result = fclose(file);
    if (xSemaphoreGive(s_storage_mutex) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static bool is_storage_file_path(const char *path)
{
    const char prefix[] = BINBOT_STORAGE_PATH "/";
    const size_t prefix_length = sizeof(prefix) - 1;
    return path != NULL && strncmp(path, prefix, prefix_length) == 0 &&
           is_safe_file_name(path + prefix_length);
}

esp_err_t storage_file_open(const char *path, const char *mode,
                            storage_file_handle_t *file)
{
    if (file == NULL || !is_storage_file_path(path) ||
        mode == NULL || strcmp(mode, "rb") != 0 || !s_mounted ||
        s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *file = NULL;

    struct storage_file *opened = calloc(1, sizeof(*opened));
    if (opened == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        free(opened);
        return ESP_ERR_TIMEOUT;
    }
    opened->file = fopen(path, mode);
    xSemaphoreGive(s_storage_mutex);
    if (opened->file == NULL) {
        free(opened);
        return ESP_ERR_NOT_FOUND;
    }
    *file = opened;
    return ESP_OK;
}

esp_err_t storage_file_read(storage_file_handle_t file, void *buffer,
                            size_t length, size_t *bytes_read)
{
    if (file == NULL || file->file == NULL || buffer == NULL ||
        length == 0 || bytes_read == NULL || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *bytes_read = fread(buffer, 1, length, file->file);
    const bool read_failed = ferror(file->file) != 0;
    xSemaphoreGive(s_storage_mutex);
    return read_failed ? ESP_FAIL : ESP_OK;
}

esp_err_t storage_file_seek(storage_file_handle_t file, long offset,
                            int whence)
{
    if (file == NULL || file->file == NULL || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int result = fseek(file->file, offset, whence);
    xSemaphoreGive(s_storage_mutex);
    return result == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_file_tell(storage_file_handle_t file, long *position)
{
    if (file == NULL || file->file == NULL || position == NULL ||
        s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const long result = ftell(file->file);
    xSemaphoreGive(s_storage_mutex);
    if (result < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    *position = result;
    return ESP_OK;
}

esp_err_t storage_file_close(storage_file_handle_t file)
{
    if (file == NULL || file->file == NULL || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int result = fclose(file->file);
    file->file = NULL;
    xSemaphoreGive(s_storage_mutex);
    free(file);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_get_file_size(const char *name, size_t *size)
{
    if (size == NULL || !s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
    esp_err_t error = make_file_path(name, path, sizeof(path));
    if (error != ESP_OK) {
        return error;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    struct stat file_stat = {0};
    if (stat(path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        error = ESP_ERR_NOT_FOUND;
    } else if (file_stat.st_size < 0) {
        error = ESP_ERR_INVALID_SIZE;
    } else {
        *size = (size_t)file_stat.st_size;
        error = ESP_OK;
    }
    xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t storage_stream_file(const char *name,
                              storage_file_chunk_callback_t callback,
                              void *context, size_t *size)
{
    if (callback == NULL || size == NULL || !s_mounted ||
        s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
    esp_err_t error = make_file_path(name, path, sizeof(path));
    if (error != ESP_OK) {
        return error;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    struct stat file_stat = {0};
    if (stat(path, &file_stat) != 0 || file_stat.st_size < 0) {
        fclose(file);
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    *size = (size_t)file_stat.st_size;
    error = ESP_OK;
    while (error == ESP_OK) {
        const size_t received = fread(s_update_buffer, 1,
                                      sizeof(s_update_buffer), file);
        if (received > 0) {
            error = callback(context, s_update_buffer, received);
        }
        if (received < sizeof(s_update_buffer)) {
            if (ferror(file) != 0) {
                error = ESP_FAIL;
            }
            break;
        }
    }
    fclose(file);
    xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t storage_list_files(storage_file_list_callback_t callback,
                             void *context)
{
    if (callback == NULL || !s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    DIR *directory = opendir(BINBOT_STORAGE_PATH);
    if (directory == NULL) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_FAIL;
    }

    esp_err_t error = ESP_OK;
    struct dirent *entry = NULL;
    while (error == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (!is_safe_file_name(entry->d_name)) {
            continue;
        }
        char path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
        error = make_file_path(entry->d_name, path, sizeof(path));
        if (error != ESP_OK) {
            break;
        }
        struct stat file_stat = {0};
        if (stat(path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            continue;
        }
        if (file_stat.st_size < 0) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        error = callback(context, entry->d_name, (size_t)file_stat.st_size);
    }
    closedir(directory);
    xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t storage_delete_file(const char *name)
{
    if (!s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
    esp_err_t error = make_file_path(name, path, sizeof(path));
    if (error != ESP_OK) {
        return error;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    error = remove(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t storage_replace_file(const char *name, size_t expected_size,
                               const uint8_t expected_sha256[32],
                               storage_stream_read_callback_t reader,
                               void *context)
{
    if (!is_safe_file_name(name) ||
        expected_size > STORAGE_MAX_FILE_SIZE || expected_sha256 == NULL ||
        reader == NULL || !s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char final_path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
    char temp_path[sizeof(BINBOT_STORAGE_PATH) + 70] = {0};
    (void)snprintf(final_path, sizeof(final_path), "%s/%s",
                   BINBOT_STORAGE_PATH, name);
    (void)snprintf(temp_path, sizeof(temp_path), "%s/.%s.part",
                   BINBOT_STORAGE_PATH, name);

    esp_err_t error = ESP_OK;
    FILE *file = fopen(temp_path, "wb");
    if (file == NULL) {
        error = ESP_FAIL;
    }

    mbedtls_md_context_t sha = {0};
    if (error == ESP_OK) {
        mbedtls_md_init(&sha);
        if (mbedtls_md_setup(&sha,
                             mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                             0) != 0 || mbedtls_md_starts(&sha) != 0) {
            error = ESP_FAIL;
        }
    }

    size_t total = 0;
    while (error == ESP_OK && total < expected_size) {
        const size_t request = expected_size - total > sizeof(s_update_buffer)
                                   ? sizeof(s_update_buffer)
                                   : expected_size - total;
        size_t received = 0;
        error = reader(context, s_update_buffer, request, &received);
        if (error != ESP_OK || received == 0 || received > request) {
            if (error == ESP_OK) {
                error = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        if (fwrite(s_update_buffer, 1, received, file) != received ||
            mbedtls_md_update(&sha, s_update_buffer, received) != 0) {
            error = ESP_FAIL;
            break;
        }
        total += received;
    }

    uint8_t actual_sha256[32] = {0};
    if (error == ESP_OK && mbedtls_md_finish(&sha, actual_sha256) != 0) {
        error = ESP_FAIL;
    }
    if (error == ESP_OK && memcmp(actual_sha256, expected_sha256,
                                  sizeof(actual_sha256)) != 0) {
        ESP_LOGW(TAG, "file hash mismatch for %s", name);
        error = ESP_ERR_INVALID_CRC;
    }
    if (file != NULL) {
        if (error == ESP_OK && (fflush(file) != 0 || fsync(fileno(file)) != 0)) {
            error = ESP_FAIL;
        }
        fclose(file);
    }
    mbedtls_md_free(&sha);

    if (error == ESP_OK) {
        char backup_path[sizeof(final_path) + 8] = {0};
        (void)snprintf(backup_path, sizeof(backup_path), "%s.bak",
                       final_path);
        (void)remove(backup_path);
        if (rename(final_path, backup_path) != 0 && access(final_path, F_OK) == 0) {
            error = ESP_FAIL;
        }
        if (error == ESP_OK && rename(temp_path, final_path) != 0) {
            error = ESP_FAIL;
        }
        if (error == ESP_OK) {
            (void)remove(backup_path);
        } else {
            (void)remove(final_path);
            (void)rename(backup_path, final_path);
        }
    }
    if (error != ESP_OK) {
        (void)remove(temp_path);
    }
    xSemaphoreGive(s_storage_mutex);
    return error;
}

static void reset_gif_update_locked(void)
{
    if (s_gif_update_fd >= 0) {
        close(s_gif_update_fd);
        s_gif_update_fd = -1;
    }
    s_gif_update_buffered = 0;
    if (s_gif_update_md_ready) {
        mbedtls_md_free(&s_gif_update_md);
        s_gif_update_md_ready = false;
    }
    s_gif_update_name[0] = '\0';
    s_gif_update_temp[0] = '\0';
    s_gif_update_expected = 0;
    s_gif_update_written = 0;
    memset(s_gif_update_sha256, 0, sizeof(s_gif_update_sha256));
}

esp_err_t storage_gif_update_begin(const char *name, size_t expected_size,
                                   const uint8_t expected_sha256[32])
{
    if (!is_safe_file_name(name) || expected_size < 13 ||
        expected_size > STORAGE_MAX_FILE_SIZE || expected_sha256 == NULL ||
        !s_mounted || s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_gif_update_fd >= 0) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    char final_path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
    if (make_file_path(name, final_path, sizeof(final_path)) != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(s_gif_update_temp, sizeof(s_gif_update_temp),
                   "%s/.%s.part", BINBOT_STORAGE_PATH, name);
    (void)remove(s_gif_update_temp);
    s_gif_update_fd = open(s_gif_update_temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (s_gif_update_fd < 0) {
        reset_gif_update_locked();
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_NO_MEM;
    }
    mbedtls_md_init(&s_gif_update_md);
    if (mbedtls_md_setup(&s_gif_update_md,
                         mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
        mbedtls_md_starts(&s_gif_update_md) != 0) {
        reset_gif_update_locked();
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_gif_update_md_ready = true;
    snprintf(s_gif_update_name, sizeof(s_gif_update_name), "%s", name);
    s_gif_update_expected = expected_size;
    s_gif_update_written = 0;
    s_gif_update_buffered = 0;
    memcpy(s_gif_update_sha256, expected_sha256, sizeof(s_gif_update_sha256));
    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

static bool flush_gif_update_locked(void)
{
    size_t offset = 0;
    while (offset < s_gif_update_buffered) {
        const ssize_t written = write(s_gif_update_fd,
                                       s_gif_update_buffer + offset,
                                       s_gif_update_buffered - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    s_gif_update_buffered = 0;
    return true;
}

esp_err_t storage_gif_update_write(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL || length == 0 || s_storage_mutex == NULL)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_gif_update_fd < 0 || length > s_gif_update_expected - s_gif_update_written) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t offset = 0;
    bool ok = true;
    while (ok && offset < length) {
        size_t count = sizeof(s_gif_update_buffer) - s_gif_update_buffered;
        if (count > length - offset) count = length - offset;
        memcpy(s_gif_update_buffer + s_gif_update_buffered, bytes + offset, count);
        s_gif_update_buffered += count;
        offset += count;
        if (s_gif_update_buffered == sizeof(s_gif_update_buffer))
            ok = flush_gif_update_locked();
    }
    ok = ok && mbedtls_md_update(&s_gif_update_md, bytes, length) == 0;
    if (ok) s_gif_update_written += length;
    xSemaphoreGive(s_storage_mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool valid_gif_file(const char *path, uint16_t width, uint16_t height)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t header[10];
    const bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                    (memcmp(header, "GIF87a", 6) == 0 ||
                     memcmp(header, "GIF89a", 6) == 0) &&
                    (((uint16_t)header[6] | (uint16_t)header[7] << 8) == width) &&
                    (((uint16_t)header[8] | (uint16_t)header[9] << 8) == height);
    fclose(file);
    return ok;
}

esp_err_t storage_gif_update_finish(uint16_t width, uint16_t height)
{
    if (s_storage_mutex == NULL || width == 0 || height == 0)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_gif_update_fd < 0 || s_gif_update_written != s_gif_update_expected) {
        xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t actual[32];
    esp_err_t error = ESP_OK;
    if (!flush_gif_update_locked() || fsync(s_gif_update_fd) != 0 ||
        mbedtls_md_finish(&s_gif_update_md, actual) != 0 ||
        memcmp(actual, s_gif_update_sha256, sizeof(actual)) != 0 ||
        !valid_gif_file(s_gif_update_temp, width, height)) {
        error = ESP_ERR_INVALID_CRC;
    }
    if (close(s_gif_update_fd) != 0) error = ESP_FAIL;
    s_gif_update_fd = -1;
    if (s_gif_update_md_ready) {
        mbedtls_md_free(&s_gif_update_md);
        s_gif_update_md_ready = false;
    }
    if (error == ESP_OK) {
        char final_path[sizeof(BINBOT_STORAGE_PATH) + 64] = {0};
        char backup_path[sizeof(final_path) + 8] = {0};
        (void)snprintf(final_path, sizeof(final_path), "%s/%s",
                       BINBOT_STORAGE_PATH, s_gif_update_name);
        (void)snprintf(backup_path, sizeof(backup_path), "%s.bak", final_path);
        (void)remove(backup_path);
        if (rename(final_path, backup_path) != 0 && access(final_path, F_OK) == 0)
            error = ESP_FAIL;
        if (error == ESP_OK && rename(s_gif_update_temp, final_path) != 0)
            error = ESP_FAIL;
        if (error == ESP_OK) {
            (void)remove(backup_path);
        } else {
            (void)remove(final_path);
            (void)rename(backup_path, final_path);
        }
    }
    if (error != ESP_OK) (void)remove(s_gif_update_temp);
    char updated_name[sizeof(s_gif_update_name)] = {0};
    if (error == ESP_OK) {
        snprintf(updated_name, sizeof(updated_name), "%s", s_gif_update_name);
    }
    const bool updated = updated_name[0] != '\0';
    reset_gif_update_locked();
    xSemaphoreGive(s_storage_mutex);
    if (updated && s_gif_update_callback) {
        s_gif_update_callback(s_gif_update_callback_context, updated_name);
    }
    return error;
}

void storage_gif_update_abort(void)
{
    if (s_storage_mutex == NULL || xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
        return;
    if (s_gif_update_temp[0]) (void)remove(s_gif_update_temp);
    reset_gif_update_locked();
    xSemaphoreGive(s_storage_mutex);
}

void storage_set_gif_update_callback(storage_gif_update_callback_t callback, void *context)
{
    s_gif_update_callback = callback;
    s_gif_update_callback_context = context;
}
