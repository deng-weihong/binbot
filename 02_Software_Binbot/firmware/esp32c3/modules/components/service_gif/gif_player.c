#include "gif_player.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "storage.h"


#define GIF_MAX_COLORS 256
#define GIF_MAX_CODE_SIZE 4096
#define GIF_STACK_SIZE (GIF_MAX_CODE_SIZE + 1)
#define GIF_DATA_BUFFER_SIZE 256

typedef struct {
    uint16_t prefix[GIF_MAX_CODE_SIZE];
    uint8_t suffix[GIF_MAX_CODE_SIZE];
    uint8_t stack[GIF_STACK_SIZE];
} gif_lzw_workspace_t;

typedef struct {
    storage_file_handle_t file;
    uint8_t block_remaining;
    uint8_t current_byte;
    uint8_t bits_remaining;
    bool block_ended;
    uint8_t buffer[GIF_DATA_BUFFER_SIZE];
    size_t buffer_position;
    size_t buffer_length;
} gif_data_reader_t;

typedef struct gif_player_t {
    storage_file_handle_t file;
    uint16_t width;
    uint16_t height;
    uint8_t global_palette[GIF_MAX_COLORS * 3];
    uint8_t frame_palette[GIF_MAX_COLORS * 3];
    uint16_t global_palette_size;
    uint8_t background_index;
    uint8_t *canvas;
    uint8_t *transparent_canvas;
    gif_lzw_workspace_t *lzw;
    long data_offset;
    uint32_t frame_delay_ms;
    uint8_t disposal_method;
    bool has_transparent_color;
    uint8_t transparent_index;
    bool rgb565_source;
    uint32_t raw_frame_count;
    uint32_t raw_frame_index;
    uint16_t raw_row_index;
    uint32_t raw_delay_ms;
} gif_player_t;

static esp_err_t read_bytes(storage_file_handle_t file, void *buffer,
                            size_t length)
{
    size_t bytes_read = 0;
    esp_err_t error = storage_file_read(file, buffer, length, &bytes_read);
    return error == ESP_OK && bytes_read == length ? ESP_OK :
           ESP_ERR_INVALID_SIZE;
}

static esp_err_t skip_sub_blocks(storage_file_handle_t file)
{
    uint8_t length;
    do {
        if (read_bytes(file, &length, 1) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (storage_file_seek(file, length, SEEK_CUR) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
    } while (length != 0);
    return ESP_OK;
}

static esp_err_t gif_data_reader_read_byte(gif_data_reader_t *reader,
                                           uint8_t *value)
{
    if (reader == NULL || value == NULL || reader->block_ended) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reader->block_remaining == 0) {
        uint8_t block_length;
        if (read_bytes(reader->file, &block_length, 1) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (block_length == 0) {
            reader->block_ended = true;
            return ESP_ERR_INVALID_SIZE;
        }
        reader->block_remaining = block_length;
    }

    if (reader->buffer_position == reader->buffer_length) {
        const size_t request = reader->block_remaining;
        if (storage_file_read(reader->file, reader->buffer, request,
                              &reader->buffer_length) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        reader->buffer_position = 0;
        if (reader->buffer_length != request) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    *value = reader->buffer[reader->buffer_position++];
    --reader->block_remaining;
    return ESP_OK;
}

static esp_err_t gif_data_reader_read_code(gif_data_reader_t *reader,
                                           uint8_t code_size,
                                           uint16_t *code)
{
    if (reader == NULL || code == NULL || code_size == 0 || code_size > 12) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t value = 0;
    for (uint8_t bit = 0; bit < code_size; ++bit) {
        if (reader->bits_remaining == 0) {
            esp_err_t error = gif_data_reader_read_byte(reader,
                                                         &reader->current_byte);
            if (error != ESP_OK) {
                return error;
            }
            reader->bits_remaining = 8;
        }
        if (reader->current_byte & (1U << (8 - reader->bits_remaining))) {
            value |= (uint16_t)1 << bit;
        }
        --reader->bits_remaining;
    }
    *code = value;
    return ESP_OK;
}

static esp_err_t gif_data_reader_drain(gif_data_reader_t *reader)
{
    if (reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    reader->bits_remaining = 0;
    while (!reader->block_ended) {
        if (reader->block_remaining != 0) {
            // The buffered reader has already advanced the file position.
            reader->buffer_position = reader->buffer_length;
            reader->block_remaining = 0;
            continue;
        }
        uint8_t block_length;
        if (read_bytes(reader->file, &block_length, 1) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (block_length == 0) {
            reader->block_ended = true;
            break;
        }
        if (storage_file_seek(reader->file, block_length, SEEK_CUR) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static esp_err_t decode_lzw(gif_player_t *player,
                            gif_data_reader_t *reader, uint16_t left,
                            uint16_t top, uint16_t width, uint16_t height,
                            bool interlaced, const uint8_t *palette,
                            uint16_t palette_size, uint8_t min_code_size,
                            uint8_t *frame, size_t frame_bytes)
{
    (void)palette;
    (void)palette_size;
    uint16_t *prefix = player->lzw->prefix;
    uint8_t *suffix = player->lzw->suffix;
    uint8_t *stack = player->lzw->stack;
    const uint16_t clear_code = (uint16_t)1 << min_code_size;
    const uint16_t end_code = clear_code + 1;
    uint16_t available = clear_code + 2;
    uint16_t code_size = min_code_size + 1;
    uint16_t old_code = 0xFFFF;
    uint8_t first = 0;
    size_t stack_size = 0;
    uint32_t output_count = 0;
    uint16_t row = 0;
    uint16_t column = 0;
    uint8_t pass = 0;

    while (output_count < (uint32_t)width * height) {
        uint16_t code;
        esp_err_t error = gif_data_reader_read_code(reader, (uint8_t)code_size,
                                                    &code);
        if (error != ESP_OK) {
            return error;
        }
        if (code == clear_code) {
            code_size = min_code_size + 1;
            available = clear_code + 2;
            old_code = 0xFFFF;
            continue;
        }
        if (code == end_code) {
            break;
        }
        if ((old_code == 0xFFFF && code >= clear_code) || code > available) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint16_t in_code = code;
        if (old_code == 0xFFFF) {
            if (stack_size >= GIF_STACK_SIZE) {
                return ESP_ERR_INVALID_SIZE;
            }
            stack[stack_size++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code == available) {
                if (stack_size >= GIF_STACK_SIZE) {
                    return ESP_ERR_INVALID_SIZE;
                }
                stack[stack_size++] = first;
                in_code = old_code;
            }
            while (in_code >= clear_code) {
                if (in_code >= available || in_code >= GIF_MAX_CODE_SIZE ||
                    stack_size >= GIF_STACK_SIZE) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                stack[stack_size++] = suffix[in_code];
                in_code = prefix[in_code];
            }
            first = (uint8_t)in_code;
            if (stack_size >= GIF_STACK_SIZE) {
                return ESP_ERR_INVALID_SIZE;
            }
            stack[stack_size++] = first;
            if (available < GIF_MAX_CODE_SIZE) {
                prefix[available] = old_code;
                suffix[available] = first;
                ++available;
                if (available == ((uint16_t)1 << code_size) && code_size < 12) {
                    ++code_size;
                }
            }
        }
        old_code = code;

        while (stack_size > 0 && output_count < (uint32_t)width * height) {
            const uint8_t index = stack[--stack_size];
            uint16_t target_row = row;
            if (interlaced) {
                static const uint8_t starts[] = {0, 4, 2, 1};
                static const uint8_t steps[] = {8, 8, 4, 2};
                target_row = (uint16_t)(starts[pass] + row * steps[pass]);
                if (target_row >= height) {
                    row = 0;
                    ++pass;
                    if (pass >= 4) {
                        return ESP_OK;
                    }
                    target_row = (uint16_t)(starts[pass] + row * steps[pass]);
                }
            }
            const uint16_t x = (uint16_t)(left + column);
            const uint16_t y = (uint16_t)(top + target_row);
            if (x < player->width && y < player->height) {
                const bool transparent = player->has_transparent_color &&
                                         index == player->transparent_index;
                size_t pixel = (size_t)y * player->width + x;
                uint8_t mask = (uint8_t)(1U << (pixel % 8));
                player->canvas[pixel] = index;
                if (transparent) player->transparent_canvas[pixel / 8] |= mask;
                else player->transparent_canvas[pixel / 8] &= (uint8_t)~mask;
            }
            ++output_count;
            ++column;
            if (column >= width) {
                column = 0;
                ++row;
            }
        }
    }

    (void)frame;
    (void)frame_bytes;
    return output_count == (uint32_t)width * height ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void convert_canvas_to_rgb565(const gif_player_t *player,
                                     const uint8_t *palette,
                                     uint8_t *frame, uint16_t frame_width,
                                     uint16_t offset_x, uint16_t offset_y,
                                     uint16_t first_row, uint16_t row_count)
{
    for (uint16_t y = first_row; y < first_row + row_count; ++y) {
        for (uint16_t x = 0; x < player->width; ++x) {
            const size_t index = (size_t)y * player->width + x;
            const uint8_t palette_index = player->canvas[index];
            const size_t output_index =
                ((size_t)(y - first_row + offset_y) * frame_width) + x + offset_x;
            if (player->transparent_canvas[index / 8] & (1U << (index % 8))) {
                // Transparent pixels use the display background, not the
                // logical GIF background color stored in the palette.
                frame[output_index * 2] = 0;
                frame[output_index * 2 + 1] = 0;
                continue;
            }
            const uint8_t *rgb = &palette[palette_index * 3];
            // The normalized upload already contains the display palette.
            // Preserve it here; a second saturation boost clips highlights.
            const uint8_t red = rgb[0];
            const uint8_t green = rgb[1];
            const uint8_t blue = rgb[2];
            const uint16_t color = (uint16_t)(((red & 0xF8) << 8) |
                                              ((green & 0xFC) << 3) |
                                              (blue >> 3));
            frame[output_index * 2] = (uint8_t)(color >> 8);
            frame[output_index * 2 + 1] = (uint8_t)color;
        }
    }
}

esp_err_t gif_player_open(const char *path, gif_player_handle_t *handle)
{
    if (path == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;
    storage_file_handle_t file = NULL;
    esp_err_t open_error = storage_file_open(path, "rb", &file);
    if (open_error != ESP_OK) {
        return open_error;
    }
    gif_player_t *player = calloc(1, sizeof(*player));
    if (player == NULL) {
        (void)storage_file_close(file);
        return ESP_ERR_NO_MEM;
    }
    player->file = file;

    uint8_t header[6];
    uint8_t logical_screen[7];
    esp_err_t error = read_bytes(file, header, sizeof(header));
    if (error == ESP_OK && memcmp(header, "GIF", 3) != 0) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error == ESP_OK) {
        error = read_bytes(file, logical_screen, sizeof(logical_screen));
    }
    if (error == ESP_OK) {
        player->width = (uint16_t)logical_screen[0] |
                        ((uint16_t)logical_screen[1] << 8);
        player->height = (uint16_t)logical_screen[2] |
                         ((uint16_t)logical_screen[3] << 8);
        player->background_index = logical_screen[5];
        const uint8_t packed = logical_screen[4];
        player->global_palette_size = packed & 0x80 ?
            (uint16_t)1 << ((packed & 0x07) + 1) : 0;
        if (player->global_palette_size == 0 || player->width == 0 ||
            player->height == 0 || player->width > 320 || player->height > 240) {
            error = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (error == ESP_OK) {
        error = read_bytes(file, player->global_palette,
                           player->global_palette_size * 3);
    }
    if (error == ESP_OK) {
        size_t pixels = (size_t)player->width * player->height;
        player->canvas = heap_caps_calloc(pixels, 1, MALLOC_CAP_INTERNAL);
        player->transparent_canvas = heap_caps_calloc((pixels + 7) / 8, 1,
                                                      MALLOC_CAP_INTERNAL);
        if (player->canvas == NULL || player->transparent_canvas == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error == ESP_OK) {
        player->lzw = heap_caps_calloc(1, sizeof(*player->lzw),
                                       MALLOC_CAP_INTERNAL);
        if (player->lzw == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error != ESP_OK) {
        heap_caps_free(player->lzw);
        heap_caps_free(player->transparent_canvas);
        free(player->canvas);
        (void)storage_file_close(file);
        free(player);
        return error;
    }
    memset(player->transparent_canvas, 0xFF,
           ((size_t)player->width * player->height + 7) / 8);
    if (storage_file_tell(file, &player->data_offset) != ESP_OK) {
        heap_caps_free(player->lzw);
        heap_caps_free(player->transparent_canvas);
        free(player->canvas);
        (void)storage_file_close(file);
        free(player);
        return ESP_ERR_INVALID_STATE;
    }
    *handle = player;
    return ESP_OK;
}

esp_err_t gif_player_rewind(gif_player_handle_t handle)
{
    if (handle == NULL || handle->file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_file_seek(handle->file, handle->data_offset, SEEK_SET) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (handle->rgb565_source) {
        handle->raw_frame_index = 0;
        handle->raw_row_index = 0;
        return ESP_OK;
    }
    memset(handle->transparent_canvas, 0xFF,
           ((size_t)handle->width * handle->height + 7) / 8);
    handle->frame_delay_ms = 100;
    handle->disposal_method = 0;
    handle->has_transparent_color = false;
    return ESP_OK;
}

static esp_err_t gif_player_next_frame_internal(
    gif_player_handle_t handle, uint8_t *frame, size_t frame_bytes,
    uint16_t canvas_width, uint16_t canvas_height, uint16_t offset_x,
    uint16_t offset_y, bool render, uint32_t *delay_ms)
{
    if (handle == NULL || delay_ms == NULL ||
        canvas_width < handle->width || canvas_height < handle->height ||
        offset_x > canvas_width - handle->width ||
        offset_y > canvas_height - handle->height ||
        (render && (frame == NULL ||
                    frame_bytes < (size_t)canvas_width * canvas_height * 2))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->rgb565_source) {
        const size_t raw_frame_bytes = (size_t)handle->width *
                                       handle->height * 2;
        if (handle->raw_frame_index >= handle->raw_frame_count) {
            if (storage_file_seek(handle->file, 0, SEEK_SET) != ESP_OK) {
                return ESP_ERR_INVALID_STATE;
            }
            handle->raw_frame_index = 0;
        }
        if (render) {
            for (uint16_t row = 0; row < handle->height; ++row) {
                uint8_t *destination = frame +
                    (((size_t)(row + offset_y) * canvas_width) + offset_x) * 2;
                size_t bytes_read = 0;
                if (storage_file_read(handle->file, destination,
                                      (size_t)handle->width * 2,
                                      &bytes_read) != ESP_OK ||
                    bytes_read != (size_t)handle->width * 2) {
                    return ESP_ERR_INVALID_SIZE;
                }
            }
        } else if (storage_file_seek(handle->file, (long)raw_frame_bytes,
                                     SEEK_CUR) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        ++handle->raw_frame_index;
        *delay_ms = handle->raw_delay_ms == 0 ? 1 : handle->raw_delay_ms;
        return ESP_OK;
    }

    while (true) {
        uint8_t block;
        if (read_bytes(handle->file, &block, 1) != ESP_OK || block == ';') {
            esp_err_t error = gif_player_rewind(handle);
            if (error != ESP_OK) {
                return error;
            }
            if (read_bytes(handle->file, &block, 1) != ESP_OK) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
        if (block == '!') {
            uint8_t label;
            if (read_bytes(handle->file, &label, 1) != ESP_OK) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (label == 0xF9) {
                uint8_t length;
                uint8_t gce[4];
                if (read_bytes(handle->file, &length, 1) != ESP_OK ||
                    length != 4 || read_bytes(handle->file, gce, 4) != ESP_OK) {
                    return ESP_ERR_INVALID_SIZE;
                }
                uint8_t terminator;
                if (read_bytes(handle->file, &terminator, 1) != ESP_OK) {
                    return ESP_ERR_INVALID_SIZE;
                }
                handle->frame_delay_ms = (uint32_t)(gce[1] | (gce[2] << 8)) * 10;
                handle->has_transparent_color = (gce[0] & 1) != 0;
                handle->transparent_index = gce[3];
                handle->disposal_method = (gce[0] >> 2) & 0x07;
            } else if (skip_sub_blocks(handle->file) != ESP_OK) {
                return ESP_ERR_INVALID_SIZE;
            }
            continue;
        }
        if (block != ',') {
            return block == 0 ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t descriptor[9];
        if (read_bytes(handle->file, descriptor, sizeof(descriptor)) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint16_t left = descriptor[0] | (descriptor[1] << 8);
        const uint16_t top = descriptor[2] | (descriptor[3] << 8);
        const uint16_t width = descriptor[4] | (descriptor[5] << 8);
        const uint16_t height = descriptor[6] | (descriptor[7] << 8);
        const uint8_t packed = descriptor[8];
        uint8_t local_palette[GIF_MAX_COLORS * 3];
        const uint8_t *palette = handle->global_palette;
        uint16_t palette_size = handle->global_palette_size;
        if (packed & 0x80) {
            palette_size = (uint16_t)1 << ((packed & 0x07) + 1);
            if (read_bytes(handle->file, local_palette, palette_size * 3) != ESP_OK) {
                return ESP_ERR_INVALID_SIZE;
            }
            palette = local_palette;
        }
        uint8_t min_code_size;
        if (read_bytes(handle->file, &min_code_size, 1) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        gif_data_reader_t data_reader = {
            .file = handle->file,
        };
        esp_err_t error = decode_lzw(handle, &data_reader, left, top,
                           width, height, (packed & 0x40) != 0, palette,
                           palette_size, min_code_size, frame, frame_bytes);
        esp_err_t drain_error = gif_data_reader_drain(&data_reader);
        if (error != ESP_OK) {
            return error;
        }
        if (drain_error != ESP_OK) {
            return drain_error;
        }
        if (render) {
            convert_canvas_to_rgb565(handle, palette, frame, canvas_width,
                                     offset_x, offset_y, 0, handle->height);
        } else memcpy(handle->frame_palette, palette, palette_size * 3);
        *delay_ms = handle->frame_delay_ms == 0 ? 100 : handle->frame_delay_ms;
        return ESP_OK;
    }
}

esp_err_t gif_player_open_rgb565(const char *path, uint16_t width,
                                  uint16_t height, uint32_t frame_count,
                                  uint32_t delay_ms,
                                  gif_player_handle_t *handle)
{
    if (path == NULL || handle == NULL || width == 0 || height == 0 ||
        frame_count == 0 || delay_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;
    storage_file_handle_t file = NULL;
    esp_err_t open_error = storage_file_open(path, "rb", &file);
    if (open_error != ESP_OK) {
        return open_error;
    }
    gif_player_t *player = calloc(1, sizeof(*player));
    if (player == NULL) {
        (void)storage_file_close(file);
        return ESP_ERR_NO_MEM;
    }
    player->file = file;
    player->width = width;
    player->height = height;
    player->rgb565_source = true;
    player->raw_frame_count = frame_count;
    player->raw_delay_ms = delay_ms;
    *handle = player;
    return ESP_OK;
}

esp_err_t gif_player_next_rgb565_rows(gif_player_handle_t handle,
                                       uint8_t *buffer, size_t buffer_bytes,
                                       uint16_t row_count, uint32_t *delay_ms)
{
    if (handle == NULL || !handle->rgb565_source || buffer == NULL ||
        delay_ms == NULL || row_count == 0 ||
        row_count > handle->height - handle->raw_row_index ||
        buffer_bytes < (size_t)row_count * handle->width * 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->raw_row_index == 0 &&
        handle->raw_frame_index >= handle->raw_frame_count) {
        if (storage_file_seek(handle->file, 0, SEEK_SET) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        handle->raw_frame_index = 0;
    }

    const size_t bytes = (size_t)row_count * handle->width * 2;
    size_t bytes_read = 0;
    if (storage_file_read(handle->file, buffer, bytes, &bytes_read) != ESP_OK ||
        bytes_read != bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    handle->raw_row_index += row_count;
    *delay_ms = 0;
    if (handle->raw_row_index == handle->height) {
        handle->raw_row_index = 0;
        ++handle->raw_frame_index;
        *delay_ms = handle->raw_delay_ms == 0 ? 1 : handle->raw_delay_ms;
    }
    return ESP_OK;
}

esp_err_t gif_player_next_frame(gif_player_handle_t handle, uint8_t *frame,
                                size_t frame_bytes, uint32_t *delay_ms)
{
    if (handle == NULL || frame_bytes != (size_t)handle->width * handle->height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    return gif_player_next_frame_internal(handle, frame, frame_bytes,
                                          handle->width, handle->height, 0, 0,
                                          true, delay_ms);
}

esp_err_t gif_player_get_size(gif_player_handle_t handle, uint16_t *width,
                              uint16_t *height)
{
    if (handle == NULL || width == NULL || height == NULL) return ESP_ERR_INVALID_ARG;
    *width = handle->width;
    *height = handle->height;
    return ESP_OK;
}

esp_err_t gif_player_next_frame_rows(gif_player_handle_t handle, uint8_t *buffer,
                                     size_t buffer_bytes, gif_player_rows_fn flush,
                                     void *context, uint32_t *delay_ms)
{
    if (handle == NULL || handle->rgb565_source || buffer == NULL || flush == NULL ||
        buffer_bytes < (size_t)handle->width * 2) return ESP_ERR_INVALID_ARG;
    esp_err_t error = gif_player_skip_frame(handle, delay_ms);
    if (error != ESP_OK) return error;
    size_t capacity = buffer_bytes / ((size_t)handle->width * 2);
    for (uint16_t row = 0; row < handle->height;) {
        uint16_t count = handle->height - row;
        if (count > capacity) count = (uint16_t)capacity;
        convert_canvas_to_rgb565(handle, handle->frame_palette, buffer,
                                 handle->width, 0, 0, row, count);
        error = flush(context, buffer, handle->width, row, count);
        if (error != ESP_OK) return error;
        row += count;
    }
    return ESP_OK;
}

esp_err_t gif_player_skip_frame(gif_player_handle_t handle,
                                uint32_t *delay_ms)
{
    if (handle == NULL || delay_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return gif_player_next_frame_internal(handle, NULL, 0, handle->width,
                                          handle->height, 0, 0, false,
                                          delay_ms);
}

esp_err_t gif_player_next_frame_to_canvas(gif_player_handle_t handle,
                                           uint8_t *frame, size_t frame_bytes,
                                           uint16_t canvas_width,
                                           uint16_t canvas_height,
                                           uint16_t offset_x,
                                           uint16_t offset_y,
                                           uint32_t *delay_ms)
{
    return gif_player_next_frame_internal(handle, frame, frame_bytes,
                                          canvas_width, canvas_height, offset_x,
                                          offset_y, true, delay_ms);
}

esp_err_t gif_player_close(gif_player_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = storage_file_close(handle->file);
    heap_caps_free(handle->lzw);
    heap_caps_free(handle->transparent_canvas);
    free(handle->canvas);
    free(handle);
    return error;
}
