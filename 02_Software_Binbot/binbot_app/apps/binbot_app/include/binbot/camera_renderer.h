#ifndef BINBOT_CAMERA_RENDERER_H
#define BINBOT_CAMERA_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct binbot_camera_renderer binbot_camera_renderer_t;

binbot_camera_renderer_t *binbot_camera_renderer_create(uint32_t width, uint32_t height,
                                                        uint32_t preview_width,
                                                        uint32_t preview_height,
                                                        bool accelerated);
void binbot_camera_renderer_destroy(binbot_camera_renderer_t *renderer);
void binbot_camera_renderer_nv12(binbot_camera_renderer_t *renderer,
                                 const uint8_t *y, uint32_t y_stride,
                                 const uint8_t *uv, uint32_t uv_stride, uint16_t *rgb565);
void binbot_camera_renderer_preview(binbot_camera_renderer_t *renderer,
                                    const uint16_t *rgb565, uint32_t *xrgb8888);
bool binbot_camera_renderer_accelerated(const binbot_camera_renderer_t *renderer);

#endif
