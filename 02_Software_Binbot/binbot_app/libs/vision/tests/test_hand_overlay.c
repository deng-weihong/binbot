#include "binbot/vision/hand_detector.h"
#include "binbot/vision/hand_overlay.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint16_t pixels[16 * 16];
    memset(pixels, 0, sizeof(pixels));
    binbot_hand_result_t result;
    memset(&result, 0, sizeof(result));
    result.presence_score = 0.9f;
    for(size_t i = 0; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        result.landmarks[i].x = 0.2f + (float)(i % 5) * 0.12f;
        result.landmarks[i].y = 0.2f + (float)(i / 5) * 0.12f;
    }
    result.gesture.left = 0.2f;
    result.gesture.top = 0.2f;
    result.gesture.right = 0.8f;
    result.gesture.bottom = 0.8f;
    binbot_hand_draw_rgb565(pixels, 16, 16, 16, &result, 0xffff, 1);

    size_t colored = 0;
    for(size_t i = 0; i < 16 * 16; ++i) {
        if(pixels[i] == 0xffff) ++colored;
    }
    assert(colored > 40);

    binbot_hand_detector_config_t config;
    memset(&config, 0, sizeof(config));
    binbot_hand_detector_t *detector = binbot_hand_detector_create(&config);
    assert(detector);
    assert(!binbot_hand_detector_is_available(detector));
    assert(binbot_hand_detector_error(detector)[0] != '\0');
    assert(binbot_hand_detector_submit_nv12(detector, NULL, 0, NULL, 0) ==
           BINBOT_HAND_SUBMIT_UNAVAILABLE);
    assert(!binbot_hand_detector_latest(detector, 0, &result));
    binbot_hand_detector_destroy(detector);

    puts("hand overlay tests passed");
    return 0;
}
