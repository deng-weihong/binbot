#include "binbot/vision/face_detector.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    char directory[] = "/tmp/binbot-rockiva-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);

    char model_path[256];
    int written = snprintf(model_path, sizeof(model_path),
                           "%s/object_detection_pfp.data", directory);
    assert(written > 0 && (size_t)written < sizeof(model_path));
    FILE *model = fopen(model_path, "wb");
    assert(model != NULL);
    assert(fputs("fake", model) >= 0);
    assert(fclose(model) == 0);

    binbot_face_detector_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = 4;
    config.height = 4;
    config.library_path = argv[1];
    config.model_directory = directory;
    config.minimum_score = 40;
    config.stable_frame_count = 1;
    config.minimum_interval_ms = 0;

    binbot_face_detector_t *detector = binbot_face_detector_create(&config);
    assert(detector != NULL);
    assert(binbot_face_detector_is_available(detector));
    assert(binbot_face_detector_error(detector)[0] == '\0');

    uint8_t y[4 * 6];
    uint8_t uv[2 * 6];
    memset(y, 1, sizeof(y));
    memset(uv, 2, sizeof(uv));
    for(int i = 0; i < 5; ++i) {
        assert(binbot_face_detector_submit_nv12(detector, y, 6, uv, 6) ==
               BINBOT_FACE_SUBMIT_ACCEPTED);
    }

    binbot_face_result_t result;
    assert(binbot_face_detector_latest(detector, 0, &result));
    assert(result.sequence == 5);
    assert(result.frame_id == 5);
    assert(result.count == 1);
    assert(result.faces[0].track_id == 42);
    assert(result.faces[0].score == 88);
    assert(result.faces[0].left == 1000);
    assert(result.faces[0].top == 2000);
    assert(result.faces[0].right == 6000);
    assert(result.faces[0].bottom == 8000);
    assert(!binbot_face_detector_latest(detector, result.sequence, &result));

    binbot_face_detector_destroy(detector);
    assert(unlink(model_path) == 0);
    assert(rmdir(directory) == 0);
    puts("face detector adapter tests passed");
    return 0;
}
