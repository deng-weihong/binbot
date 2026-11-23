#include "binbot/vision/hand_gesture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if(argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s LANDMARKS.f32 [PRESENCE]\n", argv[0]);
        return 2;
    }

    float presence = 1.0f;
    if(argc == 3) {
        char *end = NULL;
        errno = 0;
        presence = strtof(argv[2], &end);
        if(errno != 0 || end == argv[2] || *end != '\0') {
            fprintf(stderr, "invalid presence score: %s\n", argv[2]);
            return 2;
        }
    }

    FILE *file = fopen(argv[1], "rb");
    if(!file) {
        fprintf(stderr, "cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    binbot_hand_landmark_t landmarks[BINBOT_HAND_LANDMARK_COUNT];
    size_t count = fread(landmarks, sizeof(landmarks[0]),
                         BINBOT_HAND_LANDMARK_COUNT, file);
    int trailing = fgetc(file);
    fclose(file);
    if(count != BINBOT_HAND_LANDMARK_COUNT || trailing != EOF) {
        fprintf(stderr, "landmark file must contain exactly %zu bytes\n",
                sizeof(landmarks));
        return 1;
    }

    binbot_gesture_result_t result;
    bool classified = binbot_gesture_classify(landmarks, presence, &result);
    printf("classified=%s label=%s score=%.6f "
           "box=[%.6f,%.6f,%.6f,%.6f]\n",
           classified ? "true" : "false",
           binbot_gesture_label_name(result.label), result.score,
           result.left, result.top, result.right, result.bottom);
    return 0;
}
