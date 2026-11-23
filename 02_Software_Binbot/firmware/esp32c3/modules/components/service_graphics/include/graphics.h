#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define GRAPHICS_BLACK 0x0000
#define GRAPHICS_RED 0xF800
#define GRAPHICS_WHITE 0xFFFF
#define GRAPHICS_BLUE 0x5D9B
#define GRAPHICS_CYAN 0x07FF
#define GRAPHICS_GREEN 0x07E0
#define GRAPHICS_YELLOW 0xFFE0
#define GRAPHICS_DIM_BLUE 0x18A3

typedef struct graphics_context_t *graphics_handle_t;

esp_err_t graphics_init(uint16_t width, uint16_t height,
                        graphics_handle_t *handle);
esp_err_t graphics_get_info(graphics_handle_t handle, uint16_t *width,
                            uint16_t *height, size_t *frame_bytes);
esp_err_t graphics_clear(graphics_handle_t handle, uint16_t color);
esp_err_t graphics_set_pixel(graphics_handle_t handle, int16_t x, int16_t y,
                             uint16_t color);
esp_err_t graphics_draw_line(graphics_handle_t handle, int16_t x0, int16_t y0,
                             int16_t x1, int16_t y1, uint16_t color);
esp_err_t graphics_fill_rect(graphics_handle_t handle, int16_t x, int16_t y,
                             int16_t width, int16_t height, uint16_t color);
esp_err_t graphics_fill_circle(graphics_handle_t handle, int16_t center_x,
                               int16_t center_y, int16_t radius,
                               uint16_t color);
esp_err_t graphics_draw_text(graphics_handle_t handle, const char *value,
                             int16_t center_x, int16_t top_y, uint8_t scale,
                             uint16_t color);
esp_err_t graphics_draw_startup_screen(graphics_handle_t handle,
                                       uint8_t phase);
esp_err_t graphics_draw_mario_screen(graphics_handle_t handle, uint8_t phase);
esp_err_t graphics_draw_face(graphics_handle_t handle, uint8_t emotion,
                             bool soft_style);
esp_err_t graphics_draw_face_animation(graphics_handle_t handle,
                                        uint8_t emotion, bool soft_style,
                                        uint8_t animation_frame);
esp_err_t graphics_draw_weather(graphics_handle_t handle);
esp_err_t graphics_draw_ota_progress(graphics_handle_t handle, uint8_t percent,
                                      bool complete);
esp_err_t graphics_render_ota_row(uint8_t *row, size_t row_bytes,
                                  uint16_t width, uint16_t row_y,
                                  uint8_t percent, bool complete);
esp_err_t graphics_fill_color(graphics_handle_t handle, uint16_t color);
esp_err_t graphics_get_frame(graphics_handle_t handle, const uint8_t **frame,
                             size_t *frame_bytes);
esp_err_t graphics_get_frame_buffer(graphics_handle_t handle, uint8_t **frame,
                                    size_t *frame_bytes);
esp_err_t graphics_scale_rgb565_rows(const uint8_t *source,
                                      uint16_t source_width,
                                      uint16_t source_height,
                                      uint8_t *destination,
                                      size_t destination_bytes,
                                      uint16_t destination_width,
                                      uint16_t destination_height,
                                      uint16_t destination_y,
                                      uint16_t row_count);
