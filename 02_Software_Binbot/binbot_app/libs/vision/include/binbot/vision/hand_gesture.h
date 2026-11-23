#ifndef BINBOT_VISION_HAND_GESTURE_H
#define BINBOT_VISION_HAND_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_HAND_LANDMARK_COUNT = 21,
};

typedef struct {
    float x;
    float y;
    float z;
} binbot_hand_landmark_t;

typedef enum {
    BINBOT_GESTURE_NONE = 0,
    BINBOT_GESTURE_OK,
    BINBOT_GESTURE_OPEN_PALM,
    BINBOT_GESTURE_CLOSED_FIST,
    BINBOT_GESTURE_THUMB_UP,
    BINBOT_GESTURE_VICTORY,
    BINBOT_GESTURE_UNKNOWN,
} binbot_gesture_label_t;

typedef struct {
    binbot_gesture_label_t label;
    float score;
    float left;
    float top;
    float right;
    float bottom;
} binbot_gesture_result_t;

/*
 * Classify one MediaPipe hand-landmark result. Coordinates are normalized to
 * the input image, with the standard MediaPipe landmark topology. The
 * classifier intentionally uses geometry rather than a vendor classifier so
 * it can recognize OK and remain replaceable when the RKNN adapter changes.
 */
bool binbot_gesture_classify(
    const binbot_hand_landmark_t landmarks[BINBOT_HAND_LANDMARK_COUNT],
    float hand_presence_score,
    binbot_gesture_result_t *result);

typedef struct {
    uint8_t confirm_frames;
    uint8_t release_frames;
    float minimum_score;
} binbot_gesture_stabilizer_config_t;

typedef struct {
    binbot_gesture_stabilizer_config_t config;
    binbot_gesture_label_t candidate;
    uint8_t candidate_frames;
    binbot_gesture_label_t active;
    uint8_t absent_frames;
    bool initialized;
} binbot_gesture_stabilizer_t;

void binbot_gesture_stabilizer_init(
    binbot_gesture_stabilizer_t *stabilizer,
    const binbot_gesture_stabilizer_config_t *config);

/*
 * Feed one frame. Returns true only when the stable gesture changes. A
 * transition to BINBOT_GESTURE_NONE is emitted after release_frames absent or
 * low-confidence frames, preventing UI/event spam from noisy landmarks.
 */
bool binbot_gesture_stabilizer_update(
    binbot_gesture_stabilizer_t *stabilizer,
    const binbot_gesture_result_t *frame,
    binbot_gesture_label_t *changed_to);

const char *binbot_gesture_label_name(binbot_gesture_label_t label);

#ifdef __cplusplus
}
#endif

#endif
