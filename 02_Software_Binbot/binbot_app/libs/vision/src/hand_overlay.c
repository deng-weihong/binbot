#include "binbot/vision/hand_overlay.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    int x;
    int y;
} point_t;

static const uint8_t hand_connections[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {5, 9}, {9, 10}, {10, 11}, {11, 12},
    {9, 13}, {13, 14}, {14, 15}, {15, 16},
    {13, 17}, {17, 18}, {18, 19}, {19, 20}, {0, 17},
};

static int clamp_int(int value, int minimum, int maximum)
{
    if(value < minimum) return minimum;
    if(value > maximum) return maximum;
    return value;
}

static point_t project(const binbot_hand_landmark_t *landmark,
                       uint32_t width,
                       uint32_t height)
{
    point_t point;
    point.x = clamp_int((int)lroundf(landmark->x * (float)(width - 1)),
                        0, (int)width - 1);
    point.y = clamp_int((int)lroundf(landmark->y * (float)(height - 1)),
                        0, (int)height - 1);
    return point;
}

static void draw_pixel(uint16_t *pixels,
                       uint32_t width,
                       uint32_t height,
                       uint32_t stride,
                       int x,
                       int y,
                       uint16_t color,
                       uint32_t thickness)
{
    int radius = (int)(thickness - 1U);
    for(int offset_y = -radius; offset_y <= radius; ++offset_y) {
        int row = y + offset_y;
        if(row < 0 || row >= (int)height) continue;
        for(int offset_x = -radius; offset_x <= radius; ++offset_x) {
            int column = x + offset_x;
            if(column < 0 || column >= (int)width) continue;
            pixels[(size_t)row * stride + (size_t)column] = color;
        }
    }
}

static void draw_line(uint16_t *pixels,
                      uint32_t width,
                      uint32_t height,
                      uint32_t stride,
                      point_t from,
                      point_t to,
                      uint16_t color,
                      uint32_t thickness)
{
    int dx = abs(to.x - from.x);
    int step_x = from.x < to.x ? 1 : -1;
    int dy = -abs(to.y - from.y);
    int step_y = from.y < to.y ? 1 : -1;
    int error = dx + dy;
    for(;;) {
        draw_pixel(pixels, width, height, stride, from.x, from.y,
                   color, thickness);
        if(from.x == to.x && from.y == to.y) break;
        int twice = error * 2;
        if(twice >= dy) {
            error += dy;
            from.x += step_x;
        }
        if(twice <= dx) {
            error += dx;
            from.y += step_y;
        }
    }
}

static void draw_box(uint16_t *pixels,
                     uint32_t width,
                     uint32_t height,
                     uint32_t stride,
                     const binbot_gesture_result_t *gesture,
                     uint16_t color,
                     uint32_t thickness)
{
    binbot_hand_landmark_t top_left = {
        .x = gesture->left,
        .y = gesture->top,
    };
    binbot_hand_landmark_t bottom_right = {
        .x = gesture->right,
        .y = gesture->bottom,
    };
    point_t left_top = project(&top_left, width, height);
    point_t right_bottom = project(&bottom_right, width, height);
    point_t right_top = {.x = right_bottom.x, .y = left_top.y};
    point_t left_bottom = {.x = left_top.x, .y = right_bottom.y};
    draw_line(pixels, width, height, stride, left_top, right_top,
              color, thickness);
    draw_line(pixels, width, height, stride, right_top, right_bottom,
              color, thickness);
    draw_line(pixels, width, height, stride, right_bottom, left_bottom,
              color, thickness);
    draw_line(pixels, width, height, stride, left_bottom, left_top,
              color, thickness);
}

void binbot_hand_draw_rgb565(uint16_t *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride_pixels,
                             const binbot_hand_result_t *result,
                             uint16_t color,
                             uint32_t thickness)
{
    if(!pixels || !result || result->presence_score <= 0.0f ||
       width == 0 || height == 0 || stride_pixels < width || thickness == 0) {
        return;
    }

    point_t points[BINBOT_HAND_LANDMARK_COUNT];
    for(size_t i = 0; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        points[i] = project(&result->landmarks[i], width, height);
    }
    for(size_t i = 0; i < sizeof(hand_connections) / sizeof(hand_connections[0]); ++i) {
        draw_line(pixels, width, height, stride_pixels,
                  points[hand_connections[i][0]], points[hand_connections[i][1]],
                  color, thickness);
    }
    for(size_t i = 0; i < BINBOT_HAND_LANDMARK_COUNT; ++i) {
        draw_pixel(pixels, width, height, stride_pixels,
                   points[i].x, points[i].y, color, thickness + 1U);
    }
    draw_box(pixels, width, height, stride_pixels, &result->gesture,
             color, thickness);
}
