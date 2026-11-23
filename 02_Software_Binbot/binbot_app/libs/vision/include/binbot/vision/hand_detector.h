#ifndef BINBOT_VISION_HAND_DETECTOR_H
#define BINBOT_VISION_HAND_DETECTOR_H

#include "binbot/vision/hand_gesture.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frame_id;
    uint64_t sequence;
    uint64_t monotonic_ms;
    float palm_score;
    float presence_score;
    float handedness_score;
    binbot_hand_landmark_t landmarks[BINBOT_HAND_LANDMARK_COUNT];
    binbot_gesture_result_t gesture;
} binbot_hand_result_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    const char *palm_model_path;
    const char *landmark_model_path;
    float minimum_palm_score;
    float minimum_presence_score;
    uint32_t minimum_interval_ms;
} binbot_hand_detector_config_t;

typedef enum {
    BINBOT_HAND_SUBMIT_ACCEPTED = 0,
    BINBOT_HAND_SUBMIT_THROTTLED,
    BINBOT_HAND_SUBMIT_QUEUE_FULL,
    BINBOT_HAND_SUBMIT_UNAVAILABLE,
    BINBOT_HAND_SUBMIT_ERROR,
} binbot_hand_submit_status_t;

typedef struct binbot_hand_detector binbot_hand_detector_t;

binbot_hand_detector_t *binbot_hand_detector_create(
    const binbot_hand_detector_config_t *config);
void binbot_hand_detector_destroy(binbot_hand_detector_t *detector);

bool binbot_hand_detector_is_available(const binbot_hand_detector_t *detector);
const char *binbot_hand_detector_error(const binbot_hand_detector_t *detector);

binbot_hand_submit_status_t binbot_hand_detector_submit_nv12(
    binbot_hand_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride);

bool binbot_hand_detector_latest(binbot_hand_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_hand_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
