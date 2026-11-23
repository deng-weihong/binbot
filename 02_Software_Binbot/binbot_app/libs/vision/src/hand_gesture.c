#include "binbot/vision/hand_gesture.h"

#include <math.h>
#include <stddef.h>

enum {
    WRIST = 0,
    THUMB_CMC = 1,
    THUMB_MCP = 2,
    THUMB_IP = 3,
    THUMB_TIP = 4,
    INDEX_MCP = 5,
    INDEX_PIP = 6,
    INDEX_DIP = 7,
    INDEX_TIP = 8,
    MIDDLE_MCP = 9,
    MIDDLE_PIP = 10,
    MIDDLE_DIP = 11,
    MIDDLE_TIP = 12,
    RING_MCP = 13,
    RING_PIP = 14,
    RING_DIP = 15,
    RING_TIP = 16,
    PINKY_MCP = 17,
    PINKY_PIP = 18,
    PINKY_DIP = 19,
    PINKY_TIP = 20,
};

static float distance2(const binbot_hand_landmark_t *a,
                       const binbot_hand_landmark_t *b)
{
    float dx = a->x - b->x;
    float dy = a->y - b->y;
    float dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

static float distance2_xy(const binbot_hand_landmark_t *a,
                          const binbot_hand_landmark_t *b)
{
    float dx = a->x - b->x;
    float dy = a->y - b->y;
    return dx * dx + dy * dy;
}

static float distance3(const binbot_hand_landmark_t *a,
                       const binbot_hand_landmark_t *b)
{
    return sqrtf(distance2(a, b));
}

static float clamp01(float value)
{
    if(value < 0.0f) return 0.0f;
    if(value > 1.0f) return 1.0f;
    return value;
}

static float angle_cosine(const binbot_hand_landmark_t *a,
                          const binbot_hand_landmark_t *vertex,
                          const binbot_hand_landmark_t *b)
{
    float ax = a->x - vertex->x;
    float ay = a->y - vertex->y;
    float az = a->z - vertex->z;
    float bx = b->x - vertex->x;
    float by = b->y - vertex->y;
    float bz = b->z - vertex->z;
    float norm_a = sqrtf(ax * ax + ay * ay + az * az);
    float norm_b = sqrtf(bx * bx + by * by + bz * bz);
    if(norm_a < 1.0e-5f || norm_b < 1.0e-5f) return -1.0f;
    return (ax * bx + ay * by + az * bz) / (norm_a * norm_b);
}

static float palm_scale(const binbot_hand_landmark_t landmarks[])
{
    float length = sqrtf(distance2_xy(&landmarks[WRIST],
                                      &landmarks[MIDDLE_MCP]));
    float width = sqrtf(distance2_xy(&landmarks[INDEX_MCP],
                                     &landmarks[PINKY_MCP]));
    float scale = fmaxf(length, 0.65f * width);
    if(scale < 1.0e-4f) scale = 1.0e-4f;
    return scale;
}

static float finger_extension_score(const binbot_hand_landmark_t landmarks[],
                                    int mcp,
                                    int pip,
                                    int dip,
                                    int tip)
{
    float axis_x = landmarks[MIDDLE_MCP].x - landmarks[WRIST].x;
    float axis_y = landmarks[MIDDLE_MCP].y - landmarks[WRIST].y;
    float axis_z = landmarks[MIDDLE_MCP].z - landmarks[WRIST].z;
    float finger_x = landmarks[tip].x - landmarks[mcp].x;
    float finger_y = landmarks[tip].y - landmarks[mcp].y;
    float finger_z = landmarks[tip].z - landmarks[mcp].z;
    float axis_norm = sqrtf(axis_x * axis_x + axis_y * axis_y +
                            axis_z * axis_z);
    float finger_norm = sqrtf(finger_x * finger_x + finger_y * finger_y +
                              finger_z * finger_z);
    float alignment = -1.0f;
    if(axis_norm > 1.0e-5f && finger_norm > 1.0e-5f) {
        alignment = (axis_x * finger_x + axis_y * finger_y +
                     axis_z * finger_z) / (axis_norm * finger_norm);
    }

    float path = distance3(&landmarks[mcp], &landmarks[pip]) +
                 distance3(&landmarks[pip], &landmarks[dip]) +
                 distance3(&landmarks[dip], &landmarks[tip]);
    float chord_ratio = path > 1.0e-5f ? finger_norm / path : 0.0f;
    /* A straight joint has opposing vectors at the vertex (cosine -1). */
    float straightness = (1.0f - angle_cosine(&landmarks[mcp], &landmarks[pip],
                                              &landmarks[dip])) * 0.5f;
    float tip_straightness =
        (1.0f - angle_cosine(&landmarks[pip], &landmarks[dip],
                             &landmarks[tip])) * 0.5f;
    float direction_score = clamp01((alignment + 0.10f) / 0.90f);
    float chain_score = clamp01((chord_ratio - 0.55f) / 0.40f);
    return clamp01(0.55f * direction_score + 0.25f * chain_score +
                   0.10f * straightness + 0.10f * tip_straightness);
}

static float thumb_extension_score(const binbot_hand_landmark_t landmarks[])
{
    float scale = palm_scale(landmarks);
    float path = distance3(&landmarks[THUMB_MCP], &landmarks[THUMB_IP]) +
                 distance3(&landmarks[THUMB_IP], &landmarks[THUMB_TIP]);
    float chord = distance3(&landmarks[THUMB_MCP], &landmarks[THUMB_TIP]);
    float chord_ratio = path > 1.0e-5f ? chord / path : 0.0f;
    float separation = distance3(&landmarks[THUMB_TIP],
                                 &landmarks[INDEX_MCP]) / scale;
    float straightness =
        (1.0f - angle_cosine(&landmarks[THUMB_MCP], &landmarks[THUMB_IP],
                             &landmarks[THUMB_TIP])) * 0.5f;
    float separation_score = clamp01((separation - 0.45f) / 0.90f);
    float chain_score = clamp01((chord_ratio - 0.65f) / 0.30f);
    return clamp01(0.55f * separation_score + 0.25f * chain_score +
                   0.20f * straightness);
}

static void set_result(binbot_gesture_result_t *result,
                       binbot_gesture_label_t label,
                       float score,
                       const binbot_hand_landmark_t landmarks[])
{
    result->label = label;
    result->score = clamp01(score);
    result->left = result->right = landmarks[0].x;
    result->top = result->bottom = landmarks[0].y;
    for(size_t i = 1; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        if(landmarks[i].x < result->left) result->left = landmarks[i].x;
        if(landmarks[i].x > result->right) result->right = landmarks[i].x;
        if(landmarks[i].y < result->top) result->top = landmarks[i].y;
        if(landmarks[i].y > result->bottom) result->bottom = landmarks[i].y;
    }
}

bool binbot_gesture_classify(
    const binbot_hand_landmark_t landmarks[BINBOT_HAND_LANDMARK_COUNT],
    float hand_presence_score,
    binbot_gesture_result_t *result)
{
    if(!landmarks || !result) return false;
    set_result(result, BINBOT_GESTURE_UNKNOWN, 0.0f, landmarks);
    if(hand_presence_score < 0.0f) hand_presence_score = 0.0f;
    if(hand_presence_score > 1.0f) hand_presence_score = 1.0f;
    if(hand_presence_score < 0.35f) {
        result->label = BINBOT_GESTURE_NONE;
        result->score = 0.0f;
        return false;
    }

    float thumb = thumb_extension_score(landmarks);
    float index = finger_extension_score(landmarks, INDEX_MCP, INDEX_PIP,
                                         INDEX_DIP, INDEX_TIP);
    float middle = finger_extension_score(landmarks, MIDDLE_MCP, MIDDLE_PIP,
                                          MIDDLE_DIP, MIDDLE_TIP);
    float ring = finger_extension_score(landmarks, RING_MCP, RING_PIP,
                                        RING_DIP, RING_TIP);
    float pinky = finger_extension_score(landmarks, PINKY_MCP, PINKY_PIP,
                                         PINKY_DIP, PINKY_TIP);
    float palm = palm_scale(landmarks);
    float pinch = sqrtf(distance2(&landmarks[THUMB_TIP], &landmarks[INDEX_TIP])) /
                  palm;

    float ok_score = clamp01(hand_presence_score *
                             (0.45f * (1.0f - clamp01(pinch / 0.48f)) +
                              0.20f * (1.0f - index) +
                              0.15f * middle + 0.10f * ring + 0.10f * pinky));
    float open_score = clamp01(hand_presence_score *
                                (0.20f * thumb + 0.20f * index +
                                 0.20f * middle + 0.20f * ring +
                                 0.20f * pinky));
    float fist_score = clamp01(hand_presence_score *
                                (0.25f * (1.0f - thumb) +
                                 0.25f * (1.0f - index) +
                                 0.25f * (1.0f - middle) +
                                 0.15f * (1.0f - ring) +
                                 0.10f * (1.0f - pinky)));
    float thumb_up_score = clamp01(hand_presence_score *
                                   (0.45f * thumb + 0.20f * (1.0f - index) +
                                    0.20f * (1.0f - middle) +
                                    0.10f * (1.0f - ring) +
                                    0.05f * (1.0f - pinky)));
    float victory_score = clamp01(hand_presence_score *
                                  (0.35f * index + 0.35f * middle +
                                   0.15f * (1.0f - ring) +
                                   0.10f * (1.0f - pinky) +
                                   0.05f * (1.0f - thumb)));

    binbot_gesture_label_t label = BINBOT_GESTURE_UNKNOWN;
    float score = 0.0f;

    /*
     * Apply topology gates before comparing confidence values. Without these
     * gates an OK sign also scores highly as an open palm because four finger
     * tips are far from the wrist. Thresholds intentionally leave a neutral
     * band between extended and folded states.
     */
    if(pinch < 0.42f && middle > 0.58f && ring > 0.58f && pinky > 0.52f) {
        label = BINBOT_GESTURE_OK;
        score = ok_score;
    }
    else if(thumb > 0.45f && index > 0.58f && middle > 0.58f &&
            ring > 0.58f && pinky > 0.52f) {
        label = BINBOT_GESTURE_OPEN_PALM;
        score = open_score;
    }
    else if(thumb > 0.58f && index < 0.52f && middle < 0.52f &&
            ring < 0.55f && pinky < 0.58f) {
        label = BINBOT_GESTURE_THUMB_UP;
        score = thumb_up_score;
    }
    else if(index > 0.58f && middle > 0.58f && ring < 0.55f &&
            pinky < 0.58f) {
        label = BINBOT_GESTURE_VICTORY;
        score = victory_score;
    }
    else if(thumb < 0.58f && index < 0.52f && middle < 0.52f &&
            ring < 0.55f && pinky < 0.58f) {
        label = BINBOT_GESTURE_CLOSED_FIST;
        score = fist_score;
    }

    /* Require a meaningful margin over the unknown/ambiguous state. */
    if(score < 0.52f) {
        label = BINBOT_GESTURE_UNKNOWN;
    }
    set_result(result, label, score, landmarks);
    return label != BINBOT_GESTURE_UNKNOWN && label != BINBOT_GESTURE_NONE;
}

void binbot_gesture_stabilizer_init(
    binbot_gesture_stabilizer_t *stabilizer,
    const binbot_gesture_stabilizer_config_t *config)
{
    if(!stabilizer) return;
    stabilizer->config.confirm_frames = 3;
    stabilizer->config.release_frames = 4;
    stabilizer->config.minimum_score = 0.60f;
    if(config) {
        if(config->confirm_frames > 0) {
            stabilizer->config.confirm_frames = config->confirm_frames;
        }
        if(config->release_frames > 0) {
            stabilizer->config.release_frames = config->release_frames;
        }
        if(config->minimum_score > 0.0f && config->minimum_score <= 1.0f) {
            stabilizer->config.minimum_score = config->minimum_score;
        }
    }
    stabilizer->candidate = BINBOT_GESTURE_NONE;
    stabilizer->candidate_frames = 0;
    stabilizer->active = BINBOT_GESTURE_NONE;
    stabilizer->absent_frames = 0;
    stabilizer->initialized = true;
}

bool binbot_gesture_stabilizer_update(
    binbot_gesture_stabilizer_t *stabilizer,
    const binbot_gesture_result_t *frame,
    binbot_gesture_label_t *changed_to)
{
    if(!stabilizer || !frame || !changed_to) return false;
    if(!stabilizer->initialized) binbot_gesture_stabilizer_init(stabilizer, NULL);

    bool valid = frame->label != BINBOT_GESTURE_NONE &&
                 frame->label != BINBOT_GESTURE_UNKNOWN &&
                 frame->score >= stabilizer->config.minimum_score;
    if(!valid) {
        stabilizer->candidate = BINBOT_GESTURE_NONE;
        stabilizer->candidate_frames = 0;
        if(stabilizer->active != BINBOT_GESTURE_NONE) {
            if(stabilizer->absent_frames < UINT8_MAX) ++stabilizer->absent_frames;
            if(stabilizer->absent_frames >= stabilizer->config.release_frames) {
                stabilizer->active = BINBOT_GESTURE_NONE;
                stabilizer->absent_frames = 0;
                *changed_to = BINBOT_GESTURE_NONE;
                return true;
            }
        }
        return false;
    }

    stabilizer->absent_frames = 0;
    if(frame->label != stabilizer->candidate) {
        stabilizer->candidate = frame->label;
        stabilizer->candidate_frames = 1;
    }
    else if(stabilizer->candidate_frames < UINT8_MAX) {
        ++stabilizer->candidate_frames;
    }
    if(stabilizer->candidate_frames >= stabilizer->config.confirm_frames &&
       stabilizer->active != stabilizer->candidate) {
        stabilizer->active = stabilizer->candidate;
        *changed_to = stabilizer->active;
        return true;
    }
    return false;
}

const char *binbot_gesture_label_name(binbot_gesture_label_t label)
{
    switch(label) {
    case BINBOT_GESTURE_NONE: return "none";
    case BINBOT_GESTURE_OK: return "ok";
    case BINBOT_GESTURE_OPEN_PALM: return "open_palm";
    case BINBOT_GESTURE_CLOSED_FIST: return "closed_fist";
    case BINBOT_GESTURE_THUMB_UP: return "thumb_up";
    case BINBOT_GESTURE_VICTORY: return "victory";
    case BINBOT_GESTURE_UNKNOWN: return "unknown";
    }
    return "unknown";
}
