#include "cup_postprocess.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
    COCO_CLASS_COUNT = 80,
    COCO_BOTTLE_CLASS = 39,
    COCO_CUP_CLASS = 41,
    YOLO_VALUES_PER_ANCHOR = 85,
    YOLO_ANCHORS_PER_OUTPUT = 3,
    CUP_CANDIDATE_LIMIT = 128,
};

typedef struct {
    float left;
    float top;
    float right;
    float bottom;
    float score;
    binbot_cup_kind_t kind;
} cup_candidate_t;

static const uint16_t yolo_anchors[BINBOT_CUP_YOLO_OUTPUT_COUNT][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326},
};

static float tensor_value(const binbot_cup_yolo_tensor_t *tensor,
                          uint32_t channel,
                          uint32_t y,
                          uint32_t x)
{
    size_t index = (size_t)(channel / tensor->channel_block) *
                       tensor->channel_stride +
                   ((size_t)y * tensor->row_stride + x) *
                       tensor->channel_block +
                   channel % tensor->channel_block;
    return ((float)tensor->data[index] - (float)tensor->zero_point) *
           tensor->scale;
}

static int8_t tensor_quantized(const binbot_cup_yolo_tensor_t *tensor,
                               uint32_t channel,
                               uint32_t y,
                               uint32_t x)
{
    size_t index = (size_t)(channel / tensor->channel_block) *
                       tensor->channel_stride +
                   ((size_t)y * tensor->row_stride + x) *
                       tensor->channel_block +
                   channel % tensor->channel_block;
    return tensor->data[index];
}

static void insert_candidate(cup_candidate_t candidates[], size_t *count,
                             const cup_candidate_t *candidate)
{
    size_t position = 0;
    while(position < *count && candidates[position].score >= candidate->score) {
        ++position;
    }
    if(position >= CUP_CANDIDATE_LIMIT) return;
    size_t new_count = *count < CUP_CANDIDATE_LIMIT ? *count + 1U : *count;
    if(new_count > position + 1U) {
        memmove(&candidates[position + 1U], &candidates[position],
                (new_count - position - 1U) * sizeof(candidates[0]));
    }
    candidates[position] = *candidate;
    *count = new_count;
}

static float intersection_over_union(const cup_candidate_t *first,
                                     const cup_candidate_t *second)
{
    float left = fmaxf(first->left, second->left);
    float top = fmaxf(first->top, second->top);
    float right = fminf(first->right, second->right);
    float bottom = fminf(first->bottom, second->bottom);
    float intersection = fmaxf(right - left, 0.0f) *
                         fmaxf(bottom - top, 0.0f);
    float first_area = fmaxf(first->right - first->left, 0.0f) *
                       fmaxf(first->bottom - first->top, 0.0f);
    float second_area = fmaxf(second->right - second->left, 0.0f) *
                        fmaxf(second->bottom - second->top, 0.0f);
    float union_area = first_area + second_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static uint16_t project_coordinate(float coordinate, uint32_t padding,
                                   uint32_t source_size,
                                   uint32_t resized_size)
{
    float projected = (coordinate - (float)padding) *
                      (float)source_size / (float)resized_size;
    if(projected < 0.0f) projected = 0.0f;
    float maximum = source_size > 0 ? (float)(source_size - 1U) : 0.0f;
    if(projected > maximum) projected = maximum;
    return (uint16_t)lroundf(projected);
}

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
    binbot_cup_result_t *result)
{
    if(!outputs || !result || model_width == 0 || model_height == 0 ||
       source_width == 0 || source_height == 0 || resized_width == 0 ||
       resized_height == 0 || minimum_score <= 0.0f ||
       minimum_score > 1.0f || nms_threshold < 0.0f ||
       nms_threshold > 1.0f) {
        return false;
    }

    cup_candidate_t candidates[CUP_CANDIDATE_LIMIT];
    size_t candidate_count = 0;
    for(uint32_t output_index = 0;
        output_index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++output_index) {
        const binbot_cup_yolo_tensor_t *tensor = &outputs[output_index];
        if(!tensor->data || tensor->grid_width == 0 ||
           tensor->grid_height == 0 || tensor->scale <= 0.0f ||
           tensor->row_stride < tensor->grid_width ||
           tensor->channel_block == 0 ||
           tensor->channel_stride <
               tensor->row_stride * tensor->grid_height *
                   tensor->channel_block ||
           model_width % tensor->grid_width != 0 ||
           model_height % tensor->grid_height != 0) {
            return false;
        }
        float stride_x = (float)model_width / tensor->grid_width;
        float stride_y = (float)model_height / tensor->grid_height;
        for(uint32_t anchor = 0; anchor < YOLO_ANCHORS_PER_OUTPUT; ++anchor) {
            uint32_t base = anchor * YOLO_VALUES_PER_ANCHOR;
            for(uint32_t y = 0; y < tensor->grid_height; ++y) {
                for(uint32_t x = 0; x < tensor->grid_width; ++x) {
                    float object_score = tensor_value(tensor, base + 4U, y, x);
                    if(object_score < minimum_score) continue;

                    int8_t maximum_class_value = tensor_quantized(
                        tensor, base + 5U, y, x);
                    uint32_t maximum_class = 0;
                    for(uint32_t class_index = 1;
                        class_index < COCO_CLASS_COUNT; ++class_index) {
                        int8_t class_value = tensor_quantized(
                            tensor, base + 5U + class_index, y, x);
                        if(class_value > maximum_class_value) {
                            maximum_class_value = class_value;
                            maximum_class = class_index;
                        }
                    }
                    if(maximum_class != COCO_BOTTLE_CLASS &&
                       maximum_class != COCO_CUP_CLASS) {
                        continue;
                    }
                    float class_score =
                        ((float)maximum_class_value -
                         (float)tensor->zero_point) * tensor->scale;
                    float score = object_score * class_score;
                    if(score < minimum_score) continue;

                    float center_x =
                        (tensor_value(tensor, base, y, x) * 2.0f - 0.5f + x) *
                        stride_x;
                    float center_y =
                        (tensor_value(tensor, base + 1U, y, x) * 2.0f - 0.5f + y) *
                        stride_y;
                    float box_width = tensor_value(tensor, base + 2U, y, x) *
                                      2.0f;
                    float box_height = tensor_value(tensor, base + 3U, y, x) *
                                       2.0f;
                    box_width = box_width * box_width *
                                yolo_anchors[output_index][anchor * 2U];
                    box_height = box_height * box_height *
                                 yolo_anchors[output_index][anchor * 2U + 1U];
                    if(box_width <= 0.0f || box_height <= 0.0f) continue;
                    cup_candidate_t candidate = {
                        .left = center_x - box_width * 0.5f,
                        .top = center_y - box_height * 0.5f,
                        .right = center_x + box_width * 0.5f,
                        .bottom = center_y + box_height * 0.5f,
                        .score = score,
                        .kind = maximum_class == COCO_BOTTLE_CLASS
                                    ? BINBOT_CUP_KIND_BOTTLE
                                    : BINBOT_CUP_KIND_CUP,
                    };
                    insert_candidate(candidates, &candidate_count, &candidate);
                }
            }
        }
    }

    result->count = 0;
    bool suppressed[CUP_CANDIDATE_LIMIT] = {false};
    for(size_t index = 0;
        index < candidate_count &&
        result->count < BINBOT_CUP_MAX_DETECTIONS; ++index) {
        if(suppressed[index]) continue;
        const cup_candidate_t *candidate = &candidates[index];
        binbot_cup_box_t box = {
            .left = project_coordinate(candidate->left, pad_left,
                                       source_width, resized_width),
            .top = project_coordinate(candidate->top, pad_top,
                                      source_height, resized_height),
            .right = project_coordinate(candidate->right, pad_left,
                                        source_width, resized_width),
            .bottom = project_coordinate(candidate->bottom, pad_top,
                                         source_height, resized_height),
            .score = candidate->score,
            .kind = candidate->kind,
        };
        if(box.right > box.left && box.bottom > box.top) {
            result->cups[result->count++] = box;
        }
        for(size_t other = index + 1U; other < candidate_count; ++other) {
            if(!suppressed[other] &&
               candidate->kind == candidates[other].kind &&
               intersection_over_union(candidate, &candidates[other]) >
                   nms_threshold) {
                suppressed[other] = true;
            }
        }
    }
    return true;
}
