#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct gif_player_t *gif_player_handle_t;
typedef esp_err_t (*gif_player_rows_fn)(void *context, const uint8_t *rows,
                                       uint16_t width, uint16_t first_row,
                                       uint16_t row_count);

esp_err_t gif_player_open(const char *path, gif_player_handle_t *handle);
esp_err_t gif_player_get_size(gif_player_handle_t handle, uint16_t *width,
                              uint16_t *height);
esp_err_t gif_player_next_frame_rows(gif_player_handle_t handle, uint8_t *buffer,
                                     size_t buffer_bytes, gif_player_rows_fn flush,
                                     void *context, uint32_t *delay_ms);
esp_err_t gif_player_open_rgb565(const char *path, uint16_t width,
                                  uint16_t height, uint32_t frame_count,
                                  uint32_t delay_ms,
                                  gif_player_handle_t *handle);
esp_err_t gif_player_next_rgb565_rows(gif_player_handle_t handle,
                                      uint8_t *buffer, size_t buffer_bytes,
                                      uint16_t row_count, uint32_t *delay_ms);
esp_err_t gif_player_next_frame(gif_player_handle_t handle, uint8_t *frame,
                                size_t frame_bytes, uint32_t *delay_ms);
esp_err_t gif_player_skip_frame(gif_player_handle_t handle,
                                uint32_t *delay_ms);
esp_err_t gif_player_next_frame_to_canvas(gif_player_handle_t handle,
                                           uint8_t *frame, size_t frame_bytes,
                                           uint16_t canvas_width,
                                           uint16_t canvas_height,
                                           uint16_t offset_x,
                                           uint16_t offset_y,
                                           uint32_t *delay_ms);
esp_err_t gif_player_rewind(gif_player_handle_t handle);
esp_err_t gif_player_close(gif_player_handle_t handle);
