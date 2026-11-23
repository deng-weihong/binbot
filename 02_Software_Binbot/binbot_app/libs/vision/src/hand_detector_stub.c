#include "binbot/vision/hand_detector.h"

#include <stdlib.h>

struct binbot_hand_detector {
    const char *error;
};

binbot_hand_detector_t *binbot_hand_detector_create(
    const binbot_hand_detector_config_t *config)
{
    (void)config;
    binbot_hand_detector_t *detector = calloc(1, sizeof(*detector));
    if(detector) detector->error = "RKNN hand detection is disabled in this build";
    return detector;
}

void binbot_hand_detector_destroy(binbot_hand_detector_t *detector)
{
    free(detector);
}

bool binbot_hand_detector_is_available(const binbot_hand_detector_t *detector)
{
    (void)detector;
    return false;
}

const char *binbot_hand_detector_error(const binbot_hand_detector_t *detector)
{
    return detector ? detector->error : "hand detector allocation failed";
}

binbot_hand_submit_status_t binbot_hand_detector_submit_nv12(
    binbot_hand_detector_t *detector,
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
    return BINBOT_HAND_SUBMIT_UNAVAILABLE;
}

bool binbot_hand_detector_latest(binbot_hand_detector_t *detector,
                                 uint64_t after_sequence,
                                 binbot_hand_result_t *result)
{
    (void)detector;
    (void)after_sequence;
    (void)result;
    return false;
}
