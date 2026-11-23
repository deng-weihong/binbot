#include "binbot/vision/face_overlay.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_projection(void)
{
    binbot_face_box_t box = {
        .left = 2500,
        .top = 2500,
        .right = 7500,
        .bottom = 7500,
    };
    binbot_face_pixel_rect_t rect;
    assert(binbot_face_project_box(&box, 8, 8, &rect));
    assert(rect.left == 2);
    assert(rect.top == 2);
    assert(rect.right == 6);
    assert(rect.bottom == 6);

    box.left = -100;
    box.top = -200;
    box.right = 11000;
    box.bottom = 12000;
    assert(binbot_face_project_box(&box, 8, 8, &rect));
    assert(rect.left == 0 && rect.top == 0);
    assert(rect.right == 7 && rect.bottom == 7);

    box.left = 5000;
    box.right = 5000;
    assert(!binbot_face_project_box(&box, 8, 8, &rect));
}

static void test_drawing(void)
{
    uint16_t pixels[8 * 10];
    memset(pixels, 0, sizeof(pixels));
    binbot_face_result_t result;
    memset(&result, 0, sizeof(result));
    result.count = 1;
    result.faces[0].left = 2500;
    result.faces[0].top = 2500;
    result.faces[0].right = 7500;
    result.faces[0].bottom = 7500;

    binbot_face_draw_rgb565(pixels, 8, 8, 10, &result, 0x07e0, 1);
    assert(pixels[2 * 10 + 2] == 0x07e0);
    assert(pixels[2 * 10 + 6] == 0x07e0);
    assert(pixels[6 * 10 + 2] == 0x07e0);
    assert(pixels[6 * 10 + 6] == 0x07e0);
    assert(pixels[4 * 10 + 4] == 0);
    assert(pixels[0 * 10 + 8] == 0);
}

static void test_detector_degrades_cleanly(void)
{
    binbot_face_detector_config_t config = {
        .width = 640,
        .height = 480,
    };
    binbot_face_detector_t *detector = binbot_face_detector_create(&config);
    assert(detector != NULL);
    assert(!binbot_face_detector_is_available(detector));
    assert(binbot_face_detector_error(detector)[0] != '\0');
    assert(binbot_face_detector_submit_nv12(detector, NULL, 0, NULL, 0) ==
           BINBOT_FACE_SUBMIT_UNAVAILABLE);
    binbot_face_result_t result;
    assert(!binbot_face_detector_latest(detector, 0, &result));
    binbot_face_detector_destroy(detector);
}

int main(void)
{
    test_projection();
    test_drawing();
    test_detector_degrades_cleanly();
    puts("face overlay tests passed");
    return 0;
}
