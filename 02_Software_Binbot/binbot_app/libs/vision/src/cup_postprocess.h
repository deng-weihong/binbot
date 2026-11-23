#ifndef BINBOT_VISION_CUP_POSTPROCESS_H
#define BINBOT_VISION_CUP_POSTPROCESS_H

#include "binbot/vision/cup_detector.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    BINBOT_CUP_YOLO_OUTPUT_COUNT = 3,
    BINBOT_CUP_YOLO_CHANNELS = 255,
};

typedef struct {
    const int8_t *data;
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t row_stride;
    uint32_t channel_stride;
    uint32_t channel_block;
    int32_t zero_point;
    float scale;
} binbot_cup_yolo_tensor_t;

bool binbot_cup_postprocess_yolov5(
    const binbot_cup_yolo_tensor_t outputs[BINBOT_CUP_YOLO_OUTPUT_COUNT],
    uint32_t model_width,
    uint32_t model_height,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t resized_width,
    uint32_t resized_height,
    uint32_t pad_left,
    uint32_t pad_top,
    float minimum_score,
    float nms_threshold,
    binbot_cup_result_t *result);

#endif
