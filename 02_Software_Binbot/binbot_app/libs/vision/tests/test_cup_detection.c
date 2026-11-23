#include "binbot/vision/cup_detector.h"
#include "binbot/vision/cup_overlay.h"
#include "cup_postprocess.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int8_t quantize(float value, int32_t zero_point, float scale)
{
    long result = lroundf(value / scale) + zero_point;
    if(result < INT8_MIN) result = INT8_MIN;
    if(result > INT8_MAX) result = INT8_MAX;
    return (int8_t)result;
}

static void set_value(binbot_cup_yolo_tensor_t *tensor,
                      uint32_t channel,
                      uint32_t y,
                      uint32_t x,
                      float value)
{
    size_t index = (size_t)channel * tensor->channel_stride +
                   (size_t)y * tensor->row_stride + x;
    ((int8_t *)tensor->data)[index] =
        quantize(value, tensor->zero_point, tensor->scale);
}

static void configure_prediction(binbot_cup_yolo_tensor_t *tensor,
                                 uint32_t anchor,
                                 uint32_t x,
                                 uint32_t y,
                                 float width_value,
                                 float height_value,
                                 float object_score,
                                 uint32_t class_id,
                                 float class_score)
{
    uint32_t base = anchor * 85U;
    set_value(tensor, base, y, x, 0.5f);
    set_value(tensor, base + 1U, y, x, 0.5f);
    set_value(tensor, base + 2U, y, x, width_value);
    set_value(tensor, base + 3U, y, x, height_value);
    set_value(tensor, base + 4U, y, x, object_score);
    set_value(tensor, base + 5U + class_id, y, x, class_score);
}

int main(void)
{
    const uint32_t grids[BINBOT_CUP_YOLO_OUTPUT_COUNT] = {80, 40, 20};
    binbot_cup_yolo_tensor_t outputs[BINBOT_CUP_YOLO_OUTPUT_COUNT];
    memset(outputs, 0, sizeof(outputs));
    for(size_t index = 0; index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
        size_t elements = (size_t)BINBOT_CUP_YOLO_CHANNELS *
                          grids[index] * grids[index];
        int8_t *data = malloc(elements);
        assert(data);
        memset(data, INT8_MIN, elements);
        outputs[index].data = data;
        outputs[index].grid_width = grids[index];
        outputs[index].grid_height = grids[index];
        outputs[index].row_stride = grids[index];
        outputs[index].channel_stride = grids[index] * grids[index];
        outputs[index].channel_block = 1;
        outputs[index].zero_point = INT8_MIN;
        outputs[index].scale = 1.0f / 255.0f;
    }

    configure_prediction(&outputs[0], 0, 30, 30, 0.5f, 0.5f,
                         0.9f, 41, 0.8f);
    configure_prediction(&outputs[0], 1, 30, 30,
                         sqrtf(10.0f / 16.0f) * 0.5f,
                         sqrtf(13.0f / 30.0f) * 0.5f,
                         0.75f, 41, 0.8f);
    configure_prediction(&outputs[0], 2, 50, 30, 0.5f, 0.5f,
                         0.9f, 41, 0.7f);
    set_value(&outputs[0], 2U * 85U + 5U, 30, 50, 0.8f);
    configure_prediction(&outputs[0], 0, 60, 50, 0.5f, 0.5f,
                         0.85f, 39, 0.75f);

    binbot_cup_result_t result;
    memset(&result, 0, sizeof(result));
    assert(binbot_cup_postprocess_yolov5(
        outputs, 640, 640, 640, 480, 640, 480, 0, 80,
        0.25f, 0.45f, &result));
    assert(result.count == 2);
    assert(result.cups[0].kind == BINBOT_CUP_KIND_CUP);
    assert(result.cups[0].left >= 238 && result.cups[0].left <= 239);
    assert(result.cups[0].right >= 248 && result.cups[0].right <= 249);
    assert(result.cups[0].top >= 157 && result.cups[0].top <= 158);
    assert(result.cups[0].bottom >= 170 && result.cups[0].bottom <= 171);
    assert(result.cups[0].score > 0.70f && result.cups[0].score < 0.74f);
    assert(result.cups[1].kind == BINBOT_CUP_KIND_BOTTLE);
    assert(result.cups[1].left >= 478 && result.cups[1].left <= 479);
    assert(result.cups[1].right >= 488 && result.cups[1].right <= 489);
    assert(result.cups[1].top >= 317 && result.cups[1].top <= 318);
    assert(result.cups[1].bottom >= 330 && result.cups[1].bottom <= 331);
    assert(result.cups[1].score > 0.62f && result.cups[1].score < 0.66f);

    uint16_t *pixels = calloc(640U * 480U, sizeof(*pixels));
    assert(pixels);
    binbot_cup_draw_rgb565(pixels, 640, 480, 640, &result, 0xfbc0, 2);
    assert(pixels[158U * 640U + 239U] == 0xfbc0);
    assert(pixels[160U * 640U + 241U] == 0);
    assert(pixels[170U * 640U + 245U] == 0xfbc0);
    free(pixels);

    binbot_cup_detector_config_t config;
    memset(&config, 0, sizeof(config));
    binbot_cup_detector_t *detector = binbot_cup_detector_create(&config);
    assert(detector);
    assert(!binbot_cup_detector_is_available(detector));
    assert(binbot_cup_detector_error(detector)[0] != '\0');
    assert(binbot_cup_detector_submit_nv12(detector, NULL, 0, NULL, 0) ==
           BINBOT_CUP_SUBMIT_UNAVAILABLE);
    assert(!binbot_cup_detector_latest(detector, 0, &result));
    binbot_cup_detector_destroy(detector);

    for(size_t index = 0; index < BINBOT_CUP_YOLO_OUTPUT_COUNT; ++index) {
        free((void *)outputs[index].data);
    }
    puts("cup detection tests passed");
    return 0;
}
