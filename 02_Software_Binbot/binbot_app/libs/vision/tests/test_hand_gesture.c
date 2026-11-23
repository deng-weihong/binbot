#include "binbot/vision/hand_gesture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void set_point(binbot_hand_landmark_t points[], int index, float x, float y)
{
    points[index].x = x;
    points[index].y = y;
    points[index].z = 0.0f;
}

static void make_hand(binbot_hand_landmark_t points[],
                      bool thumb,
                      bool index,
                      bool middle,
                      bool ring,
                      bool pinky)
{
    memset(points, 0, sizeof(binbot_hand_landmark_t) * BINBOT_HAND_LANDMARK_COUNT);
    set_point(points, 0, 0.50f, 0.75f);

    set_point(points, 1, 0.42f, 0.69f);
    set_point(points, 2, 0.36f, 0.62f);
    set_point(points, 3, thumb ? 0.27f : 0.43f, thumb ? 0.55f : 0.65f);
    set_point(points, 4, thumb ? 0.18f : 0.47f, thumb ? 0.48f : 0.69f);

    const float mcp_x[] = {0.43f, 0.50f, 0.57f, 0.64f};
    const int mcp[] = {5, 9, 13, 17};
    const int pip[] = {6, 10, 14, 18};
    const int dip[] = {7, 11, 15, 19};
    const int tip[] = {8, 12, 16, 20};
    const bool extended[] = {index, middle, ring, pinky};
    for(int i = 0; i < 4; ++i) {
        float x = mcp_x[i];
        set_point(points, mcp[i], x, 0.66f);
        if(extended[i]) {
            set_point(points, pip[i], x, 0.54f);
            set_point(points, dip[i], x, 0.42f);
            set_point(points, tip[i], x, 0.30f);
        }
        else {
            set_point(points, pip[i], x + 0.025f, 0.69f);
            set_point(points, dip[i], x + 0.045f, 0.73f);
            set_point(points, tip[i], x + 0.060f, 0.71f);
        }
    }
}

static void transform_hand(binbot_hand_landmark_t points[],
                           float radians,
                           bool mirror)
{
    float cosine = cosf(radians);
    float sine = sinf(radians);
    for(int i = 0; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        float x = points[i].x - 0.5f;
        float y = points[i].y - 0.5f;
        if(mirror) x = -x;
        points[i].x = 0.5f + x * cosine - y * sine;
        points[i].y = 0.5f + x * sine + y * cosine;
    }
}

static void assert_label(binbot_hand_landmark_t points[],
                         binbot_gesture_label_t expected)
{
    binbot_gesture_result_t result;
    bool classified = binbot_gesture_classify(points, 1.0f, &result);
    if(!classified || result.label != expected) {
        fprintf(stderr, "expected %s, got %s (score %.3f)\n",
                binbot_gesture_label_name(expected),
                binbot_gesture_label_name(result.label), result.score);
    }
    assert(classified);
    assert(result.label == expected);
    assert(result.score >= 0.52f);
    assert(result.left <= result.right);
    assert(result.top <= result.bottom);
}

static void test_labels(void)
{
    binbot_hand_landmark_t points[BINBOT_HAND_LANDMARK_COUNT];
    binbot_gesture_result_t result;

    make_hand(points, true, true, true, true, true);
    assert_label(points, BINBOT_GESTURE_OPEN_PALM);

    transform_hand(points, 0.65f, true);
    assert_label(points, BINBOT_GESTURE_OPEN_PALM);

    make_hand(points, false, false, false, false, false);
    assert_label(points, BINBOT_GESTURE_CLOSED_FIST);

    make_hand(points, true, false, false, false, false);
    assert_label(points, BINBOT_GESTURE_THUMB_UP);

    make_hand(points, false, true, true, false, false);
    assert_label(points, BINBOT_GESTURE_VICTORY);

    make_hand(points, true, false, true, true, true);
    set_point(points, 4, 0.47f, 0.30f);
    set_point(points, 8, 0.47f, 0.30f);
    assert_label(points, BINBOT_GESTURE_OK);

    assert(!binbot_gesture_classify(points, 0.1f, &result));
    assert(result.label == BINBOT_GESTURE_NONE);
}

static void test_stabilizer(void)
{
    binbot_gesture_stabilizer_t stabilizer;
    binbot_gesture_stabilizer_config_t config = {
        .confirm_frames = 3,
        .release_frames = 2,
        .minimum_score = 0.6f,
    };
    binbot_gesture_stabilizer_init(&stabilizer, &config);
    binbot_gesture_result_t frame = {
        .label = BINBOT_GESTURE_OK,
        .score = 0.9f,
    };
    binbot_gesture_label_t changed = BINBOT_GESTURE_NONE;
    assert(!binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));
    assert(!binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));
    assert(binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));
    assert(changed == BINBOT_GESTURE_OK);
    assert(!binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));

    frame.label = BINBOT_GESTURE_UNKNOWN;
    frame.score = 0.0f;
    assert(!binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));
    assert(binbot_gesture_stabilizer_update(&stabilizer, &frame, &changed));
    assert(changed == BINBOT_GESTURE_NONE);
}

int main(void)
{
    test_labels();
    test_stabilizer();
    assert(strcmp(binbot_gesture_label_name(BINBOT_GESTURE_OK), "ok") == 0);
    puts("hand gesture tests passed");
    return 0;
}
