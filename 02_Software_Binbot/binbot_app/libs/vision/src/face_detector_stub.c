#include "binbot/vision/face_detector.h"

#include <stdlib.h>

struct binbot_face_detector {
    const char *error;
};

binbot_face_detector_t *binbot_face_detector_create(
    const binbot_face_detector_config_t *config)
{
    (void)config;
    binbot_face_detector_t *detector = calloc(1, sizeof(*detector));
    if(detector) detector->error = "RockIVA is disabled in this build";
    return detector;
}

void binbot_face_detector_destroy(binbot_face_detector_t *detector)
{
    free(detector);
}

bool binbot_face_detector_is_available(const binbot_face_detector_t *detector)
{
    (void)detector;
    return false;
}

const char *binbot_face_detector_error(const binbot_face_detector_t *detector)
{
    return detector ? detector->error : "face detector allocation failed";
}

binbot_face_submit_status_t binbot_face_detector_submit_nv12(
    binbot_face_detector_t *detector,
    const uint8_t *y_plane,
    uint32_t y_stride,
    const uint8_t *uv_plane,
    uint32_t uv_stride)
{
    (void)detector;
    (void)y_plane;
    (void)y_stride;
    (void)uv_plane;
    (void)uv_stride;
    return BINBOT_FACE_SUBMIT_UNAVAILABLE;
}

bool binbot_face_detector_latest(binbot_face_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_face_result_t *result)
{
    (void)detector;
    (void)after_sequence;
    (void)result;
    return false;
}
