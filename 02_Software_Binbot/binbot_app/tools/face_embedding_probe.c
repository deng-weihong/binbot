#include "binbot/vision/face_embedding.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FACE_RGB_BYTES = 112 * 112 * 3 };

static unsigned char *read_rgb(const char *path)
{
    unsigned char *data = malloc(FACE_RGB_BYTES);
    if(!data) return NULL;
    if(!path) {
        memset(data, 127, FACE_RGB_BYTES);
        return data;
    }
    FILE *file = fopen(path, "rb");
    if(!file || fread(data, 1, FACE_RGB_BYTES, file) != FACE_RGB_BYTES) {
        fprintf(stderr, "input must contain exactly %d RGB bytes: %s\n",
                FACE_RGB_BYTES, path);
        if(file) fclose(file);
        free(data);
        return NULL;
    }
    if(fgetc(file) != EOF) {
        fprintf(stderr, "input has more than %d RGB bytes\n", FACE_RGB_BYTES);
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    return data;
}

int main(int argc, char **argv)
{
    if(argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s recognition.rknn [112x112.rgb] [embedding.f32]\n",
                argv[0]);
        return 2;
    }
    binbot_face_embedder_t *embedder = binbot_face_embedder_create(argv[1]);
    if(!binbot_face_embedder_is_available(embedder)) {
        fprintf(stderr, "embedder unavailable: %s\n",
                binbot_face_embedder_error(embedder));
        binbot_face_embedder_destroy(embedder);
        return 1;
    }
    unsigned char *rgb = read_rgb(argc >= 3 ? argv[2] : NULL);
    if(!rgb) {
        binbot_face_embedder_destroy(embedder);
        return 1;
    }
    float embedding[BINBOT_FACE_EMBEDDING_DIM];
    if(!binbot_face_embedder_run_rgb(embedder, rgb, embedding)) {
        fprintf(stderr, "inference failed: %s\n",
                binbot_face_embedder_error(embedder));
        free(rgb);
        binbot_face_embedder_destroy(embedder);
        return 1;
    }
    double norm = 0.0;
    float minimum = embedding[0];
    float maximum = embedding[0];
    for(size_t i = 0; i < BINBOT_FACE_EMBEDDING_DIM; ++i) {
        norm += (double)embedding[i] * embedding[i];
        if(embedding[i] < minimum) minimum = embedding[i];
        if(embedding[i] > maximum) maximum = embedding[i];
    }
    printf("embedding_dim=%u norm=%g min=%g max=%g first=",
           BINBOT_FACE_EMBEDDING_DIM, norm, minimum, maximum);
    for(size_t i = 0; i < 8; ++i) printf("%s%.7g", i ? "," : "[", embedding[i]);
    puts("]");
    if(argc == 4) {
        FILE *file = fopen(argv[3], "wb");
        if(!file || fwrite(embedding, sizeof(embedding), 1, file) != 1) {
            fprintf(stderr, "cannot write %s: %s\n", argv[3], strerror(errno));
            if(file) fclose(file);
            free(rgb);
            binbot_face_embedder_destroy(embedder);
            return 1;
        }
        fclose(file);
        printf("reference_feature=%s\n", argv[3]);
    }
    free(rgb);
    binbot_face_embedder_destroy(embedder);
    return 0;
}
