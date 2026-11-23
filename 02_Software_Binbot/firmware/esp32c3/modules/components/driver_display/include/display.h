#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DISPLAY_DEVICE_GC9A01A = 0,
    DISPLAY_DEVICE_ILI9342C,
} display_device_t;

typedef struct display_context_t *display_handle_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    size_t frame_bytes;
} display_info_t;

esp_err_t display_init(display_device_t device, display_handle_t *handle);
esp_err_t display_deinit(display_handle_t handle);
esp_err_t display_get_info(display_handle_t handle, display_info_t *info);
esp_err_t display_clear(display_handle_t handle, uint16_t color);
esp_err_t display_flush(display_handle_t handle, const uint8_t *frame,
                        size_t frame_bytes);
esp_err_t display_flush_region(display_handle_t handle, const uint8_t *frame,
                               size_t frame_bytes, uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height);
