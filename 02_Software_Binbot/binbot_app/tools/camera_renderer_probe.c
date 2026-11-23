#include "binbot/camera_renderer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 640, HEIGHT = 480, PREVIEW_WIDTH = 536, PREVIEW_HEIGHT = 680 };

static double milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(10);
    uint8_t *nv12 = malloc(WIDTH * HEIGHT * 3 / 2);
    uint16_t *rgb = malloc(WIDTH * HEIGHT * sizeof(*rgb));
    uint32_t *preview = malloc(PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(*preview));
    assert(nv12 && rgb && preview);
    for(unsigned cycle = 0; cycle < 3; ++cycle) {
        puts("Creating CMA renderer");
        binbot_camera_renderer_t *r = binbot_camera_renderer_create(
            WIDTH, HEIGHT, PREVIEW_WIDTH, PREVIEW_HEIGHT, true);
        assert(r && binbot_camera_renderer_accelerated(r));
        double started = milliseconds();
        for(unsigned frame = 0; frame < 60; ++frame) {
            static const unsigned colors[][4] = {
                {0, 128, 128, 0}, {255, 128, 128, 0xf8fcf8},
                {76, 85, 255, 0xf80000}, {29, 255, 107, 0x0000f8}
            };
            const unsigned *color = colors[frame % 4];
            memset(nv12, color[0], WIDTH * HEIGHT);
            for(unsigned i = WIDTH * HEIGHT; i < WIDTH * HEIGHT * 3 / 2; i += 2) {
                nv12[i] = color[1]; nv12[i + 1] = color[2];
            }
            binbot_camera_renderer_nv12(r, nv12, WIDTH, nv12 + WIDTH * HEIGHT, WIDTH, rgb);
            binbot_camera_renderer_preview(r, rgb, preview);
            assert(binbot_camera_renderer_accelerated(r));
            uint32_t expected = color[3];
            for(unsigned i = 0; i < PREVIEW_WIDTH * PREVIEW_HEIGHT; ++i) {
                if((preview[i] & 0xf8fcf8U) != expected) {
                    printf("Unexpected frame=%u pixel=%u rgb565=%04x preview=%08x expected=%08x\n",
                           frame, i, rgb[0], preview[i], expected);
                    return 1;
                }
            }
        }
        printf("Cycle %u: conversion + scale + copies %.2f ms/frame\n", cycle, (milliseconds() - started) / 60);
        for(unsigned y = 0; y < HEIGHT; ++y)
            for(unsigned x = 0; x < WIDTH; ++x)
                rgb[y * WIDTH + x] = x < WIDTH / 3 ? 0xf800 : x < WIDTH * 2 / 3 ? 0x07e0 : 0x001f;
        binbot_camera_renderer_preview(r, rgb, preview);
        for(unsigned y = 0; y < PREVIEW_HEIGHT; ++y) {
            if((preview[y * PREVIEW_WIDTH] & 0xf8fcf8U) != 0xf80000U ||
               (preview[y * PREVIEW_WIDTH + PREVIEW_WIDTH / 2] & 0xf8fcf8U) != 0x00fc00U ||
               (preview[y * PREVIEW_WIDTH + PREVIEW_WIDTH - 1] & 0xf8fcf8U) != 0x0000f8U) {
                printf("Unexpected stripe row=%u left=%08x center=%08x right=%08x\n", y,
                       preview[y * PREVIEW_WIDTH], preview[y * PREVIEW_WIDTH + PREVIEW_WIDTH / 2],
                       preview[y * PREVIEW_WIDTH + PREVIEW_WIDTH - 1]);
                return 1;
            }
        }
        binbot_camera_renderer_destroy(r);
        puts("CMA renderer released; black/white, RGB channels and center crop passed");
    }
    free(preview); free(rgb); free(nv12);
    return 0;
}
