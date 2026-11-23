#include "binbot/vision/face_recognizer.h"

#include <stdlib.h>
#include <string.h>

struct binbot_face_recognizer {
    binbot_face_recognition_result_t result;
    char error[96];
};

binbot_face_recognizer_t *binbot_face_recognizer_create(
    const binbot_face_recognizer_config_t *config)
{
    (void)config;
    binbot_face_recognizer_t *recognizer = calloc(1, sizeof(*recognizer));
    if(recognizer) {
        strcpy(recognizer->error, "RKNN face recognition is disabled in this build");
        recognizer->result.state = BINBOT_FACE_MATCH_NO_REFERENCE;
    }
    return recognizer;
}

void binbot_face_recognizer_destroy(binbot_face_recognizer_t *recognizer)
{
    free(recognizer);
}

bool binbot_face_recognizer_is_available(
    const binbot_face_recognizer_t *recognizer)
{
    (void)recognizer;
    return false;
}

const char *binbot_face_recognizer_error(
    const binbot_face_recognizer_t *recognizer)
{
    return recognizer ? recognizer->error : "recognizer is null";
}

binbot_face_recognition_result_t binbot_face_recognizer_status(
    const binbot_face_recognizer_t *recognizer)
{
    binbot_face_recognition_result_t result;
    memset(&result, 0, sizeof(result));
    result.state = BINBOT_FACE_MATCH_NO_REFERENCE;
    if(recognizer) result = recognizer->result;
    return result;
}

binbot_face_recognition_submit_status_t binbot_face_recognizer_submit_nv12(
    binbot_face_recognizer_t *recognizer, const uint8_t *y_plane,
    uint32_t y_stride, const uint8_t *uv_plane, uint32_t uv_stride,
    const binbot_face_box_t *face)
{
    (void)recognizer; (void)y_plane; (void)y_stride; (void)uv_plane;
    (void)uv_stride; (void)face;
    return BINBOT_FACE_RECOGNITION_UNAVAILABLE;
}

bool binbot_face_recognizer_latest(binbot_face_recognizer_t *recognizer,
                                   uint64_t after_sequence,
                                   binbot_face_recognition_result_t *result)
{
    if(!recognizer || !result || recognizer->result.sequence <= after_sequence) {
        return false;
    }
    *result = recognizer->result;
    return true;
}
