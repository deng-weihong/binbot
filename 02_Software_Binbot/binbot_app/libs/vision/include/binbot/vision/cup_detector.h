#ifndef BINBOT_VISION_CUP_DETECTOR_H
#define BINBOT_VISION_CUP_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_CUP_MAX_DETECTIONS = 8,
};

typedef enum {
    BINBOT_CUP_KIND_BOTTLE = 0,
    BINBOT_CUP_KIND_CUP,
} binbot_cup_kind_t;

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
    float score;
    binbot_cup_kind_t kind;
} binbot_cup_box_t;

typedef struct {
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t frame_id;
    size_t count;
    binbot_cup_box_t cups[BINBOT_CUP_MAX_DETECTIONS];
} binbot_cup_result_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    const char *model_path;
    float minimum_score;
    float nms_threshold;
    uint32_t minimum_interval_ms;
} binbot_cup_detector_config_t;

typedef enum {
    BINBOT_CUP_SUBMIT_ACCEPTED = 0,
    BINBOT_CUP_SUBMIT_THROTTLED,
    BINBOT_CUP_SUBMIT_QUEUE_FULL,
    BINBOT_CUP_SUBMIT_UNAVAILABLE,
    BINBOT_CUP_SUBMIT_ERROR,
} binbot_cup_submit_status_t;

typedef struct binbot_cup_detector binbot_cup_detector_t;

binbot_cup_detector_t *binbot_cup_detector_create(
    const binbot_cup_detector_config_t *config);
void binbot_cup_detector_destroy(binbot_cup_detector_t *detector);

bool binbot_cup_detector_is_available(const binbot_cup_detector_t *detector);
const char *binbot_cup_detector_error(const binbot_cup_detector_t *detector);

binbot_cup_submit_status_t binbot_cup_detector_submit_nv12(
    binbot_cup_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride);

bool binbot_cup_detector_latest(binbot_cup_detector_t *detector,
                                uint64_t after_sequence,
                                binbot_cup_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
