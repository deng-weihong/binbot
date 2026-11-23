#include "binbot/vision/face_overlay.h"

static int32_t clamp_coordinate(int32_t value)
{
    if(value < 0) return 0;
    if(value > BINBOT_FACE_COORDINATE_SCALE) return BINBOT_FACE_COORDINATE_SCALE;
    return value;
}

bool binbot_face_project_box(const binbot_face_box_t *box,
                             uint32_t width,
                             uint32_t height,
                             binbot_face_pixel_rect_t *rect)
{
    if(!box || !rect || width == 0 || height == 0) return false;

    int32_t left = clamp_coordinate(box->left);
    int32_t top = clamp_coordinate(box->top);
    int32_t right = clamp_coordinate(box->right);
    int32_t bottom = clamp_coordinate(box->bottom);
    if(right <= left || bottom <= top) return false;

    rect->left = (uint32_t)(((uint64_t)(uint32_t)left * width) /
                            BINBOT_FACE_COORDINATE_SCALE);
    rect->top = (uint32_t)(((uint64_t)(uint32_t)top * height) /
                           BINBOT_FACE_COORDINATE_SCALE);
    rect->right = (uint32_t)(((uint64_t)(uint32_t)right * width) /
                             BINBOT_FACE_COORDINATE_SCALE);
    rect->bottom = (uint32_t)(((uint64_t)(uint32_t)bottom * height) /
                              BINBOT_FACE_COORDINATE_SCALE);

    if(rect->left >= width) rect->left = width - 1;
    if(rect->top >= height) rect->top = height - 1;
    if(rect->right >= width) rect->right = width - 1;
    if(rect->bottom >= height) rect->bottom = height - 1;
    return rect->right > rect->left && rect->bottom > rect->top;
}

static void draw_rect(uint16_t *pixels,
                      uint32_t stride,
                      const binbot_face_pixel_rect_t *rect,
                      uint16_t color,
                      uint32_t thickness)
{
    uint32_t max_thickness = (rect->right - rect->left + 1) / 2;
    uint32_t vertical_limit = (rect->bottom - rect->top + 1) / 2;
    if(vertical_limit < max_thickness) max_thickness = vertical_limit;
    if(thickness > max_thickness) thickness = max_thickness;
    if(thickness == 0) return;

    for(uint32_t inset = 0; inset < thickness; ++inset) {
        uint32_t left = rect->left + inset;
        uint32_t right = rect->right - inset;
        uint32_t top = rect->top + inset;
        uint32_t bottom = rect->bottom - inset;

        for(uint32_t x = left; x <= right; ++x) {
            pixels[(size_t)top * stride + x] = color;
            pixels[(size_t)bottom * stride + x] = color;
        }
        for(uint32_t y = top; y <= bottom; ++y) {
            pixels[(size_t)y * stride + left] = color;
            pixels[(size_t)y * stride + right] = color;
        }
    }
}

void binbot_face_draw_rgb565(uint16_t *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels,
                             const binbot_face_result_t *result,
                             uint16_t color,
                             uint32_t thickness)
{
    if(!pixels || !result || width == 0 || height == 0 ||
       stride_pixels < width || thickness == 0) {
        return;
    }

    size_t count = result->count;
    if(count > BINBOT_FACE_MAX_DETECTIONS) count = BINBOT_FACE_MAX_DETECTIONS;
    for(size_t i = 0; i < count; ++i) {
        binbot_face_pixel_rect_t rect;
        if(binbot_face_project_box(&result->faces[i], width, height, &rect)) {
            draw_rect(pixels, stride_pixels, &rect, color, thickness);
        }
    }
}
