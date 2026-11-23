#ifndef BINBOT_VISION_CUP_OVERLAY_H
#define BINBOT_VISION_CUP_OVERLAY_H

#include "binbot/vision/cup_detector.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void binbot_cup_draw_rgb565(uint16_t *pixels,
                            uint32_t width,
                            uint32_t height,
                            uint32_t stride_pixels,
                            const binbot_cup_result_t *result,
                            uint16_t color,
                            uint32_t thickness);

#ifdef __cplusplus
}
#endif

#endif
