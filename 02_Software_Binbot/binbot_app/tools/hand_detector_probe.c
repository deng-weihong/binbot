#include "binbot/vision/hand_detector.h"
#include "binbot/vision/hand_overlay.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool parse_dimension(const char *text, uint16_t *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if(errno != 0 || end == text || *end != '\0' || parsed == 0 ||
       parsed > UINT16_MAX || (parsed & 1U) != 0) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static uint8_t clamp_u8(int value)
{
    if(value < 0) return 0;
    if(value > 255) return 255;
    return (uint8_t)value;
}

static void nv12_to_rgb565(const uint8_t *frame,
                           uint16_t width,
                           uint16_t height,
                           uint16_t *pixels)
{
    const uint8_t *uv = frame + (size_t)width * height;
    for(uint32_t y = 0; y < height; ++y) {
        for(uint32_t x = 0; x < width; ++x) {
            int luminance = frame[(size_t)y * width + x];
            size_t uv_index = (size_t)(y / 2U) * width + (x & ~1U);
            int u = (int)uv[uv_index] - 128;
            int v = (int)uv[uv_index + 1U] - 128;
            uint8_t red = clamp_u8(luminance + ((359 * v) >> 8));
            uint8_t green = clamp_u8(
                luminance - ((88 * u + 183 * v) >> 8));
            uint8_t blue = clamp_u8(luminance + ((454 * u) >> 8));
            pixels[(size_t)y * width + x] =
                (uint16_t)(((red & 0xf8U) << 8) |
                           ((green & 0xfcU) << 3) | (blue >> 3));
        }
    }
}

static bool write_ppm(const char *path,
                      const uint16_t *pixels,
                      uint16_t width,
                      uint16_t height)
{
    FILE *file = fopen(path, "wb");
    if(!file) return false;
    if(fprintf(file, "P6\n%u %u\n255\n", width, height) < 0) {
        fclose(file);
        return false;
    }
    for(size_t i = 0; i < (size_t)width * height; ++i) {
        uint16_t pixel = pixels[i];
        uint8_t rgb[3] = {
            (uint8_t)((((pixel >> 11) & 0x1fU) * 255U + 15U) / 31U),
            (uint8_t)((((pixel >> 5) & 0x3fU) * 255U + 31U) / 63U),
            (uint8_t)(((pixel & 0x1fU) * 255U + 15U) / 31U),
        };
        if(fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    if(argc < 6 || argc > 7) {
        fprintf(stderr,
                "usage: %s WIDTH HEIGHT FRAME.nv12 PALM.rknn LANDMARK.rknn "
                "[OUTPUT.ppm]\n",
                argv[0]);
        return 2;
    }
    uint16_t width;
    uint16_t height;
    if(!parse_dimension(argv[1], &width) ||
       !parse_dimension(argv[2], &height)) {
        fputs("WIDTH and HEIGHT must be positive even integers\n", stderr);
        return 2;
    }
    size_t frame_size = (size_t)width * height * 3U / 2U;
    uint8_t *frame = malloc(frame_size);
    if(!frame) return 1;
    FILE *file = fopen(argv[3], "rb");
    if(!file || fread(frame, 1, frame_size, file) != frame_size ||
       fgetc(file) != EOF) {
        fprintf(stderr, "FRAME.nv12 must contain exactly %zu bytes\n", frame_size);
        if(file) fclose(file);
        free(frame);
        return 1;
    }
    fclose(file);

    binbot_hand_detector_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = width;
    config.height = height;
    config.palm_model_path = argv[4];
    config.landmark_model_path = argv[5];
    config.minimum_palm_score = 0.5f;
    config.minimum_presence_score = 0.5f;
    config.minimum_interval_ms = 1;
    binbot_hand_detector_t *detector = binbot_hand_detector_create(&config);
    if(!binbot_hand_detector_is_available(detector)) {
        fprintf(stderr, "hand detector unavailable: %s\n",
                binbot_hand_detector_error(detector));
        binbot_hand_detector_destroy(detector);
        free(frame);
        return 1;
    }

    const uint8_t *uv = frame + (size_t)width * height;
    binbot_hand_submit_status_t status = binbot_hand_detector_submit_nv12(
        detector, frame, width, uv, width);
    if(status != BINBOT_HAND_SUBMIT_ACCEPTED) {
        fprintf(stderr, "frame submission failed: %d\n", status);
        binbot_hand_detector_destroy(detector);
        free(frame);
        return 1;
    }

    binbot_hand_result_t result;
    bool received = false;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
    for(int attempt = 0; attempt < 500; ++attempt) {
        if(binbot_hand_detector_latest(detector, 0, &result)) {
            received = true;
            break;
        }
        nanosleep(&delay, NULL);
    }
    if(!received) {
        fprintf(stderr, "timed out waiting for hand result: %s\n",
                binbot_hand_detector_error(detector));
        binbot_hand_detector_destroy(detector);
        free(frame);
        return 1;
    }

    printf("frame=%u palm=%.6f presence=%.6f handedness=%.6f "
           "label=%s score=%.6f sequence=%llu\n",
           result.frame_id, result.palm_score, result.presence_score,
           result.handedness_score,
           binbot_gesture_label_name(result.gesture.label),
           result.gesture.score, (unsigned long long)result.sequence);
    for(size_t i = 0; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        printf("landmark[%zu]=%.6f,%.6f,%.6f\n", i,
               result.landmarks[i].x, result.landmarks[i].y,
               result.landmarks[i].z);
    }
    if(argc == 7) {
        uint16_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
        if(!pixels) {
            binbot_hand_detector_destroy(detector);
            free(frame);
            return 1;
        }
        nv12_to_rgb565(frame, width, height, pixels);
        binbot_hand_draw_rgb565(pixels, width, height, width,
                                &result, 0xffe0, 2);
        bool written = write_ppm(argv[6], pixels, width, height);
        free(pixels);
        if(!written) {
            fprintf(stderr, "cannot create %s: %s\n", argv[6], strerror(errno));
            binbot_hand_detector_destroy(detector);
            free(frame);
            return 1;
        }
        printf("overlay=%s\n", argv[6]);
    }
    binbot_hand_detector_destroy(detector);
    free(frame);
    return 0;
}
