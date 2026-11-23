#ifndef BINBOT_VISION_FACE_EMBEDDING_H
#define BINBOT_VISION_FACE_EMBEDDING_H

#include "binbot/vision/face_matcher.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct binbot_face_embedder binbot_face_embedder_t;

binbot_face_embedder_t *binbot_face_embedder_create(const char *model_path);
void binbot_face_embedder_destroy(binbot_face_embedder_t *embedder);
bool binbot_face_embedder_is_available(const binbot_face_embedder_t *embedder);
const char *binbot_face_embedder_error(const binbot_face_embedder_t *embedder);

/* Input is packed RGB888, row-major, exactly 112 x 112 pixels. */
bool binbot_face_embedder_run_rgb(
    binbot_face_embedder_t *embedder,
    const unsigned char *rgb,
    float embedding[BINBOT_FACE_EMBEDDING_DIM]);

#ifdef __cplusplus
}
#endif

#endif
