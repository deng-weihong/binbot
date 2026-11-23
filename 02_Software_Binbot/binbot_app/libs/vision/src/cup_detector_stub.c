#include "binbot/vision/cup_detector.h"

#include <stdlib.h>

struct binbot_cup_detector {
    const char *error;
};

binbot_cup_detector_t *binbot_cup_detector_create(
    const binbot_cup_detector_config_t *config)
{
    (void)config;
    binbot_cup_detector_t *detector = calloc(1, sizeof(*detector));
    if(detector) detector->error = "RKNN cup detection is disabled in this build";
    return detector;
}

void binbot_cup_detector_destroy(binbot_cup_detector_t *detector)
{
    free(detector);
}

bool binbot_cup_detector_is_available(const binbot_cup_detector_t *detector)
{
    (void)detector;
    return false;
}

const char *binbot_cup_detector_error(const binbot_cup_detector_t *detector)
{
    return detector ? detector->error : "cup detector allocation failed";
}

binbot_cup_submit_status_t binbot_cup_detector_submit_nv12(
    binbot_cup_detector_t *detector,
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
    return BINBOT_CUP_SUBMIT_UNAVAILABLE;
}

bool binbot_cup_detector_latest(binbot_cup_detector_t *detector,
                                uint64_t after_sequence,
                                binbot_cup_result_t *result)
{
    (void)detector;
    (void)after_sequence;
    (void)result;
    return false;
}
