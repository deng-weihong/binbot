#ifndef BINBOT_VISION_FACE_DETECTOR_H
#define BINBOT_VISION_FACE_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_FACE_MAX_DETECTIONS = 16,
    BINBOT_FACE_COORDINATE_SCALE = 10000,
};

typedef struct {
    uint32_t track_id;
    uint8_t score;
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} binbot_face_box_t;

typedef struct {
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t frame_id;
    size_t count;
    binbot_face_box_t faces[BINBOT_FACE_MAX_DETECTIONS];
} binbot_face_result_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    const char *library_path;
    const char *model_directory;
    uint8_t minimum_score;
    uint8_t stable_frame_count;
    uint32_t minimum_interval_ms;
} binbot_face_detector_config_t;

typedef enum {
    BINBOT_FACE_SUBMIT_ACCEPTED = 0,
    BINBOT_FACE_SUBMIT_THROTTLED,
    BINBOT_FACE_SUBMIT_QUEUE_FULL,
    BINBOT_FACE_SUBMIT_UNAVAILABLE,
    BINBOT_FACE_SUBMIT_ERROR,
} binbot_face_submit_status_t;

typedef struct binbot_face_detector binbot_face_detector_t;

binbot_face_detector_t *binbot_face_detector_create(
    const binbot_face_detector_config_t *config);
void binbot_face_detector_destroy(binbot_face_detector_t *detector);

bool binbot_face_detector_is_available(const binbot_face_detector_t *detector);
const char *binbot_face_detector_error(const binbot_face_detector_t *detector);

binbot_face_submit_status_t binbot_face_detector_submit_nv12(
    binbot_face_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride);

bool binbot_face_detector_latest(binbot_face_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_face_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
