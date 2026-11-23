#include "binbot/vision/face_matcher.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_normalize_and_cosine(void)
{
    float first[BINBOT_FACE_EMBEDDING_DIM] = {0};
    float second[BINBOT_FACE_EMBEDDING_DIM] = {0};
    first[0] = 3.0f;
    first[1] = 4.0f;
    second[0] = 3.0f;
    second[1] = 4.0f;
    assert(binbot_face_embedding_normalize(first));
    assert(binbot_face_embedding_normalize(second));
    assert(fabsf(first[0] - 0.6f) < 0.0001f);
    assert(fabsf(binbot_face_cosine_similarity(first, second) - 1.0f) < 0.0001f);
    second[0] = -second[0];
    second[1] = -second[1];
    assert(binbot_face_cosine_similarity(first, second) < -0.99f);
}

static void test_confirmation_and_track_reset(void)
{
    binbot_face_matcher_t matcher;
    binbot_face_matcher_init(&matcher, 0.8f, 2);
    float reference[BINBOT_FACE_EMBEDDING_DIM] = {0};
    reference[0] = 1.0f;
    assert(binbot_face_matcher_set_reference(&matcher, reference));
    float matching[BINBOT_FACE_EMBEDDING_DIM] = {0};
    matching[0] = 2.0f;
    float score = 0.0f;
    assert(binbot_face_matcher_compare(&matcher, 7, matching, &score) ==
           BINBOT_FACE_MATCH_PENDING);
    assert(score > 0.99f);
    assert(binbot_face_matcher_compare(&matcher, 7, matching, &score) ==
           BINBOT_FACE_MATCH_CONFIRMED);
    assert(binbot_face_matcher_compare(&matcher, 8, matching, &score) ==
           BINBOT_FACE_MATCH_PENDING);

    float other[BINBOT_FACE_EMBEDDING_DIM] = {0};
    other[1] = 1.0f;
    assert(binbot_face_matcher_compare(&matcher, 8, other, &score) ==
           BINBOT_FACE_MATCH_MISMATCH);
    assert(isnan(binbot_face_cosine_similarity(NULL, other)));
}

int main(void)
{
    test_normalize_and_cosine();
    test_confirmation_and_track_reset();
    puts("face matcher tests passed");
    return 0;
}
