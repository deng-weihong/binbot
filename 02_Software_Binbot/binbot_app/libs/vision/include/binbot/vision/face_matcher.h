#ifndef BINBOT_VISION_FACE_MATCHER_H
#define BINBOT_VISION_FACE_MATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINBOT_FACE_EMBEDDING_DIM 128U

typedef enum {
    BINBOT_FACE_MATCH_NO_REFERENCE = 0,
    BINBOT_FACE_MATCH_INVALID_VECTOR,
    BINBOT_FACE_MATCH_MISMATCH,
    BINBOT_FACE_MATCH_PENDING,
    BINBOT_FACE_MATCH_CONFIRMED,
} binbot_face_match_state_t;

typedef struct {
    float reference[BINBOT_FACE_EMBEDDING_DIM];
    bool has_reference;
    float threshold;
    uint8_t confirm_frames;
    uint8_t consecutive_matches;
    uint32_t last_track_id;
} binbot_face_matcher_t;

/* Normalizes a vector in place. Returns false for zero/non-finite vectors. */
bool binbot_face_embedding_normalize(float vector[BINBOT_FACE_EMBEDDING_DIM]);

/* Both inputs must be normalized; the function still rejects invalid values. */
float binbot_face_cosine_similarity(
    const float first[BINBOT_FACE_EMBEDDING_DIM],
    const float second[BINBOT_FACE_EMBEDDING_DIM]);

void binbot_face_matcher_init(binbot_face_matcher_t *matcher,
                              float threshold,
                              uint8_t confirm_frames);

bool binbot_face_matcher_set_reference(
    binbot_face_matcher_t *matcher,
    const float reference[BINBOT_FACE_EMBEDDING_DIM]);

/* The file is exactly 128 little-endian/native float32 values. */
bool binbot_face_matcher_load_reference(binbot_face_matcher_t *matcher,
                                        const char *path);

binbot_face_match_state_t binbot_face_matcher_compare(
    binbot_face_matcher_t *matcher,
    uint32_t track_id,
    const float embedding[BINBOT_FACE_EMBEDDING_DIM],
    float *score);

#ifdef __cplusplus
}
#endif

#endif
