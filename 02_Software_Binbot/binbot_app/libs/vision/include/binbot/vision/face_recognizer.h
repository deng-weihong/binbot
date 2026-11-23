#ifndef BINBOT_VISION_FACE_RECOGNIZER_H
#define BINBOT_VISION_FACE_RECOGNIZER_H

#include "binbot/vision/face_detector.h"
#include "binbot/vision/face_matcher.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    const char *model_path;
    const char *reference_feature_path;
    float match_threshold;
    uint8_t confirm_frames;
    uint32_t minimum_interval_ms;
    float crop_margin;
    const char *reference_capture_path;
    uint8_t reference_capture_samples;
} binbot_face_recognizer_config_t;

typedef struct {
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t frame_id;
    uint32_t track_id;
    float score;
    bool has_reference;
    binbot_face_match_state_t state;
} binbot_face_recognition_result_t;

typedef enum {
    BINBOT_FACE_RECOGNITION_ACCEPTED = 0,
    BINBOT_FACE_RECOGNITION_THROTTLED,
    BINBOT_FACE_RECOGNITION_QUEUE_FULL,
    BINBOT_FACE_RECOGNITION_UNAVAILABLE,
    BINBOT_FACE_RECOGNITION_ERROR,
} binbot_face_recognition_submit_status_t;

typedef struct binbot_face_recognizer binbot_face_recognizer_t;

binbot_face_recognizer_t *binbot_face_recognizer_create(
    const binbot_face_recognizer_config_t *config);
void binbot_face_recognizer_destroy(binbot_face_recognizer_t *recognizer);
bool binbot_face_recognizer_is_available(
    const binbot_face_recognizer_t *recognizer);
const char *binbot_face_recognizer_error(
    const binbot_face_recognizer_t *recognizer);

binbot_face_recognition_result_t binbot_face_recognizer_status(
    const binbot_face_recognizer_t *recognizer);

binbot_face_recognition_submit_status_t binbot_face_recognizer_submit_nv12(
    binbot_face_recognizer_t *recognizer,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride,
    const binbot_face_box_t *face);

bool binbot_face_recognizer_latest(
    binbot_face_recognizer_t *recognizer,
    uint64_t after_sequence,
    binbot_face_recognition_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
