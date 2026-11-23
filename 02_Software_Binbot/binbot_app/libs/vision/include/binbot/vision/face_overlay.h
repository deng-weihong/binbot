#ifndef BINBOT_VISION_FACE_OVERLAY_H
#define BINBOT_VISION_FACE_OVERLAY_H

#include "binbot/vision/face_detector.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
} binbot_face_pixel_rect_t;

bool binbot_face_project_box(const binbot_face_box_t *box,
                             uint32_t width,
                             uint32_t height,
                             binbot_face_pixel_rect_t *rect);

void binbot_face_draw_rgb565(uint16_t *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels,
                             const binbot_face_result_t *result,
                             uint16_t color,
                             uint32_t thickness);

#ifdef __cplusplus
}
#endif

#endif
