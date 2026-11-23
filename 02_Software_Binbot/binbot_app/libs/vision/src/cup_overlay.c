#include "binbot/vision/cup_overlay.h"

#include <stddef.h>

static void fill_rect(uint16_t *pixels, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t left, uint32_t top,
                      uint32_t right, uint32_t bottom, uint16_t color)
{
    if(left >= width || top >= height) return;
    if(right >= width) right = width - 1U;
    if(bottom >= height) bottom = height - 1U;
    if(right < left || bottom < top) return;
    for(uint32_t y = top; y <= bottom; ++y) {
        for(uint32_t x = left; x <= right; ++x) {
            pixels[(size_t)y * stride + x] = color;
        }
    }
}

void binbot_cup_draw_rgb565(uint16_t *pixels,
                            uint32_t width,
                            uint32_t height,
                            uint32_t stride_pixels,
                            const binbot_cup_result_t *result,
                            uint16_t color,
                            uint32_t thickness)
{
    if(!pixels || !result || width == 0 || height == 0 ||
       stride_pixels < width || thickness == 0) {
        return;
    }
    for(size_t index = 0; index < result->count; ++index) {
        const binbot_cup_box_t *box = &result->cups[index];
        uint32_t left = box->left;
        uint32_t top = box->top;
        uint32_t right = box->right;
        uint32_t bottom = box->bottom;
        if(right <= left || bottom <= top) continue;
        uint32_t horizontal_bottom = top + thickness - 1U;
        if(horizontal_bottom > bottom) horizontal_bottom = bottom;
        fill_rect(pixels, width, height, stride_pixels,
                  left, top, right, horizontal_bottom, color);
        uint32_t bottom_top = bottom >= thickness - 1U
                                  ? bottom - (thickness - 1U) : 0;
        fill_rect(pixels, width, height, stride_pixels,
                  left, bottom_top, right, bottom, color);
        uint32_t vertical_right = left + thickness - 1U;
        if(vertical_right > right) vertical_right = right;
        fill_rect(pixels, width, height, stride_pixels,
                  left, top, vertical_right, bottom, color);
        uint32_t right_left = right >= thickness - 1U
                                  ? right - (thickness - 1U) : 0;
        fill_rect(pixels, width, height, stride_pixels,
                  right_left, top, right, bottom, color);
    }
}
