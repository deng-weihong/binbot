#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BINBOT_STORAGE_PATH "/storage"

typedef esp_err_t (*storage_stream_read_callback_t)(
    void *context, uint8_t *buffer, size_t buffer_size, size_t *bytes_read);
typedef esp_err_t (*storage_file_chunk_callback_t)(
    void *context, const uint8_t *buffer, size_t length);
typedef esp_err_t (*storage_file_list_callback_t)(
    void *context, const char *name, size_t size);
typedef void (*storage_gif_update_callback_t)(void *context, const char *name);

typedef struct storage_file *storage_file_handle_t;

esp_err_t storage_mount(void);
esp_err_t storage_unmount(void);
esp_err_t storage_open_file(const char *path, const char *mode, FILE **file);
esp_err_t storage_close_file(FILE *file);
esp_err_t storage_file_open(const char *path, const char *mode,
                            storage_file_handle_t *file);
esp_err_t storage_file_read(storage_file_handle_t file, void *buffer,
                            size_t length, size_t *bytes_read);
esp_err_t storage_file_seek(storage_file_handle_t file, long offset,
                            int whence);
esp_err_t storage_file_tell(storage_file_handle_t file, long *position);
esp_err_t storage_file_close(storage_file_handle_t file);
esp_err_t storage_get_file_size(const char *name, size_t *size);
esp_err_t storage_stream_file(const char *name,
                              storage_file_chunk_callback_t callback,
                              void *context, size_t *size);
esp_err_t storage_list_files(storage_file_list_callback_t callback,
                             void *context);
esp_err_t storage_delete_file(const char *name);
esp_err_t storage_replace_file(const char *name, size_t expected_size,
                               const uint8_t expected_sha256[32],
                               storage_stream_read_callback_t reader,
                               void *context);

/* Push-based GIF update used by the UART component link. Each call holds the
 * storage mutex only for its filesystem operation, so UART reception can
 * continue between chunks. */
esp_err_t storage_gif_update_begin(const char *name, size_t expected_size,
                                   const uint8_t expected_sha256[32]);
esp_err_t storage_gif_update_write(const uint8_t *bytes, size_t length);
esp_err_t storage_gif_update_finish(uint16_t width, uint16_t height);
void storage_gif_update_abort(void);
void storage_set_gif_update_callback(storage_gif_update_callback_t callback, void *context);
