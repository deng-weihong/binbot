#include "binbot/vision/face_matcher.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool vector_is_finite(const float vector[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!vector) return false;
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        if(!isfinite(vector[i])) return false;
    }
    return true;
}

bool binbot_face_embedding_normalize(float vector[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!vector_is_finite(vector)) return false;
    double squared_norm = 0.0;
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        squared_norm += (double)vector[i] * (double)vector[i];
    }
    if(!isfinite(squared_norm) || squared_norm <= 1.0e-12) return false;
    float inverse_norm = 1.0f / sqrtf((float)squared_norm);
    if(!isfinite(inverse_norm)) return false;
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        vector[i] *= inverse_norm;
    }
    return vector_is_finite(vector);
}

float binbot_face_cosine_similarity(
    const float first[BINBOT_FACE_EMBEDDING_DIM],
    const float second[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!vector_is_finite(first) || !vector_is_finite(second)) return NAN;
    double dot = 0.0;
    double first_norm = 0.0;
    double second_norm = 0.0;
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        dot += (double)first[i] * (double)second[i];
        first_norm += (double)first[i] * (double)first[i];
        second_norm += (double)second[i] * (double)second[i];
    }
    if(first_norm <= 1.0e-12 || second_norm <= 1.0e-12) return NAN;
    double score = dot / sqrt(first_norm * second_norm);
    if(!isfinite(score)) return NAN;
    if(score < -1.0) score = -1.0;
    if(score > 1.0) score = 1.0;
    return (float)score;
}

void binbot_face_matcher_init(binbot_face_matcher_t *matcher,
                              float threshold,
                              uint8_t confirm_frames)
{
    if(!matcher) return;
    memset(matcher, 0, sizeof(*matcher));
    matcher->threshold = threshold > -1.0f && threshold < 1.0f
                             ? threshold : 0.65f;
    matcher->confirm_frames = confirm_frames ? confirm_frames : 1U;
}

bool binbot_face_matcher_set_reference(
    binbot_face_matcher_t *matcher,
    const float reference[BINBOT_FACE_EMBEDDING_DIM])
{
    if(!matcher || !reference) return false;
    float normalized[BINBOT_FACE_EMBEDDING_DIM];
    memcpy(normalized, reference, sizeof(normalized));
    if(!binbot_face_embedding_normalize(normalized)) return false;
    memcpy(matcher->reference, normalized, sizeof(matcher->reference));
    matcher->has_reference = true;
    matcher->consecutive_matches = 0;
    matcher->last_track_id = 0;
    return true;
}

bool binbot_face_matcher_load_reference(binbot_face_matcher_t *matcher,
                                        const char *path)
{
    if(!matcher || !path || !path[0]) return false;
    FILE *file = fopen(path, "rb");
    if(!file) return false;
    float reference[BINBOT_FACE_EMBEDDING_DIM];
    bool ok = fread(reference, sizeof(reference), 1, file) == 1;
    if(ok) {
        int extra = fgetc(file);
        ok = extra == EOF;
    }
    if(fclose(file) != 0) ok = false;
    if(!ok) return false;
    return binbot_face_matcher_set_reference(matcher, reference);
}

binbot_face_match_state_t binbot_face_matcher_compare(
    binbot_face_matcher_t *matcher,
    uint32_t track_id,
    const float embedding[BINBOT_FACE_EMBEDDING_DIM],
    float *score)
{
    if(score) *score = NAN;
    if(!matcher || !matcher->has_reference) {
        if(matcher) matcher->consecutive_matches = 0;
        return BINBOT_FACE_MATCH_NO_REFERENCE;
    }
    if(!embedding || !vector_is_finite(embedding)) {
        matcher->consecutive_matches = 0;
        return BINBOT_FACE_MATCH_INVALID_VECTOR;
    }
    float current[BINBOT_FACE_EMBEDDING_DIM];
    memcpy(current, embedding, sizeof(current));
    if(!binbot_face_embedding_normalize(current)) {
        matcher->consecutive_matches = 0;
        return BINBOT_FACE_MATCH_INVALID_VECTOR;
    }
    float similarity = binbot_face_cosine_similarity(matcher->reference, current);
    if(score) *score = similarity;
    if(!isfinite(similarity) || similarity < matcher->threshold) {
        matcher->consecutive_matches = 0;
        matcher->last_track_id = track_id;
        return BINBOT_FACE_MATCH_MISMATCH;
    }

    if(track_id != matcher->last_track_id) matcher->consecutive_matches = 0;
    matcher->last_track_id = track_id;
    if(matcher->consecutive_matches < UINT8_MAX) ++matcher->consecutive_matches;
    return matcher->consecutive_matches >= matcher->confirm_frames
               ? BINBOT_FACE_MATCH_CONFIRMED
               : BINBOT_FACE_MATCH_PENDING;
}
