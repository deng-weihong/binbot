#include "graphics.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

esp_err_t graphics_scale_rgb565_rows(const uint8_t *source,
                                      uint16_t source_width,
                                      uint16_t source_height,
                                      uint8_t *destination,
                                      size_t destination_bytes,
                                      uint16_t destination_width,
                                      uint16_t destination_height,
                                      uint16_t destination_y,
                                      uint16_t row_count)
{
    if (source == NULL || destination == NULL || source_width == 0 ||
        source_height == 0 || destination_width == 0 ||
        destination_height == 0 || row_count == 0 ||
        destination_y >= destination_height ||
        row_count > destination_height - destination_y ||
        destination_bytes < (size_t)destination_width * row_count * 2) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint16_t row = 0; row < row_count; ++row) {
        const uint16_t source_y = (uint16_t)(((uint32_t)(destination_y + row) *
                                              source_height) /
                                             destination_height);
        for (uint16_t x = 0; x < destination_width; ++x) {
            const uint16_t source_x = (uint16_t)(((uint32_t)x * source_width) /
                                                 destination_width);
            const size_t source_offset =
                ((size_t)source_y * source_width + source_x) * 2;
            const size_t destination_offset =
                ((size_t)row * destination_width + x) * 2;
            destination[destination_offset] = source[source_offset];
            destination[destination_offset + 1] = source[source_offset + 1];
        }
    }
    return ESP_OK;
}

struct graphics_context_t {
    uint16_t width;
    uint16_t height;
    size_t frame_bytes;
    uint8_t *frame;
};

static void pixel(graphics_handle_t handle, int16_t x, int16_t y,
                  uint16_t color)
{
    if (x < 0 || x >= (int16_t)handle->width ||
        y < 0 || y >= (int16_t)handle->height) {
        return;
    }
    size_t offset = ((size_t)y * handle->width + (size_t)x) * 2;
    handle->frame[offset] = (uint8_t)(color >> 8);
    handle->frame[offset + 1] = (uint8_t)color;
}

static void line(graphics_handle_t handle, int16_t x0, int16_t y0,
                 int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx = (int16_t)abs(x1 - x0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t dy = (int16_t)-abs(y1 - y0);
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t error = dx + dy;
    while (true) {
        pixel(handle, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int16_t doubled = (int16_t)(2 * error);
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void fill_rect(graphics_handle_t handle, int16_t x, int16_t y,
                      int16_t width, int16_t height, uint16_t color)
{
    for (int16_t py = y; py < y + height; ++py) {
        for (int16_t px = x; px < x + width; ++px) {
            pixel(handle, px, py, color);
        }
    }
}

static void fill_circle(graphics_handle_t handle, int16_t center_x,
                        int16_t center_y, int16_t radius, uint16_t color)
{
    for (int16_t y = -radius; y <= radius; ++y) {
        for (int16_t x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                pixel(handle, center_x + x, center_y + y, color);
            }
        }
    }
}

static void fill_round_rect(graphics_handle_t handle, int16_t x, int16_t y,
                            int16_t width, int16_t height, int16_t radius,
                            uint16_t color)
{
    for (int16_t py = 0; py < height; ++py) {
        for (int16_t px = 0; px < width; ++px) {
            int16_t dx = 0;
            int16_t dy = 0;
            if (px < radius) {
                dx = radius - px;
            } else if (px >= width - radius) {
                dx = px - (width - radius - 1);
            }
            if (py < radius) {
                dy = radius - py;
            } else if (py >= height - radius) {
                dy = py - (height - radius - 1);
            }
            if ((dx == 0 && dy == 0) || dx * dx + dy * dy <= radius * radius) {
                pixel(handle, x + px, y + py, color);
            }
        }
    }
}

typedef struct {
    int16_t x;
    int16_t y;
} graphics_point_t;

static void fill_polygon(graphics_handle_t handle,
                         const graphics_point_t *points, size_t count,
                         uint16_t color)
{
    if (count < 3) {
        return;
    }

    int16_t min_y = points[0].y;
    int16_t max_y = points[0].y;
    for (size_t index = 1; index < count; ++index) {
        if (points[index].y < min_y) {
            min_y = points[index].y;
        }
        if (points[index].y > max_y) {
            max_y = points[index].y;
        }
    }

    for (int16_t y = min_y; y <= max_y; ++y) {
        int16_t intersections[32];
        size_t intersection_count = 0;
        for (size_t index = 0; index < count; ++index) {
            const graphics_point_t first = points[index];
            const graphics_point_t second = points[(index + 1) % count];
            const bool crosses_scanline =
                (first.y <= y && second.y > y) ||
                (second.y <= y && first.y > y);
            if (!crosses_scanline || intersection_count >= 32) {
                continue;
            }
            const int32_t numerator = (int32_t)(y - first.y) *
                                      (second.x - first.x);
            const int32_t denominator = second.y - first.y;
            intersections[intersection_count++] =
                (int16_t)(first.x + numerator / denominator);
        }

        for (size_t left = 0; left < intersection_count; ++left) {
            for (size_t right = left + 1; right < intersection_count; ++right) {
                if (intersections[right] < intersections[left]) {
                    const int16_t value = intersections[left];
                    intersections[left] = intersections[right];
                    intersections[right] = value;
                }
            }
        }
        for (size_t index = 0; index + 1 < intersection_count; index += 2) {
            for (int16_t x = intersections[index];
                 x <= intersections[index + 1]; ++x) {
                pixel(handle, x, y, color);
            }
        }
    }
}

static void draw_reference_eye(graphics_handle_t handle, bool mirrored)
{
    // This is the Figma round-face-anger eye path reduced to pixel geometry.
    static const graphics_point_t source[] = {
        {13, 53}, {13, 42}, {15, 32}, {20, 24}, {27, 18}, {36, 16},
        {49, 16}, {60, 18}, {70, 24}, {78, 33}, {83, 43}, {84, 51},
        {83, 56}, {81, 59}, {76, 59}, {68, 56}, {60, 55}, {52, 54},
        {44, 54}, {40, 55}, {32, 55}, {25, 57}, {18, 59}, {14, 58},
    };
    graphics_point_t mapped[sizeof(source) / sizeof(source[0])];
    const float angle = (mirrored ? -11.0f : 11.0f) * 0.0174532925f;
    const float pivot_x = mirrored ? 151.0f : 49.0f;
    const float pivot_y = 43.0f;
    const float cosine = cosf(angle);
    const float sine = sinf(angle);

    for (size_t index = 0; index < sizeof(source) / sizeof(source[0]); ++index) {
        float x = mirrored ? 200.0f - source[index].x : source[index].x;
        float y = source[index].y;
        const float translated_x = x - pivot_x;
        const float translated_y = y - pivot_y;
        const float rotated_x = translated_x * cosine - translated_y * sine + pivot_x;
        const float rotated_y = translated_x * sine + translated_y * cosine + pivot_y;
        mapped[index].x = (int16_t)(120.0f + (rotated_x - 100.0f) * 0.95f);
        mapped[index].y = (int16_t)(71.0f + rotated_y * (92.0f / 94.0f));
    }
    fill_polygon(handle, mapped, sizeof(mapped) / sizeof(mapped[0]),
                 GRAPHICS_WHITE);
}

static void draw_reference_anger_face(graphics_handle_t handle)
{
    draw_reference_eye(handle, false);
    draw_reference_eye(handle, true);

    const int16_t mouth_x0 = 107;
    const int16_t mouth_x1 = 133;
    const int16_t mouth_y = 148;
    fill_rect(handle, mouth_x0 + 2, mouth_y - 2, mouth_x1 - mouth_x0 - 4,
              5, GRAPHICS_WHITE);
    fill_circle(handle, mouth_x0 + 2, mouth_y, 2, GRAPHICS_WHITE);
    fill_circle(handle, mouth_x1 - 2, mouth_y, 2, GRAPHICS_WHITE);
}

static void draw_reference_anger_blink(graphics_handle_t handle,
                                       uint8_t blink_phase)
{
    if (blink_phase == 0) {
        draw_reference_anger_face(handle);
        return;
    }

    const int16_t eye_y = 110;
    const int16_t eye_height = blink_phase == 2 ? 3 : 5;
    fill_round_rect(handle, 44, eye_y - eye_height / 2, 50, eye_height, 2,
                    GRAPHICS_WHITE);
    fill_round_rect(handle, 146, eye_y - eye_height / 2, 50, eye_height, 2,
                    GRAPHICS_WHITE);
    fill_rect(handle, 109, 146, 22, 5, GRAPHICS_WHITE);
}

esp_err_t graphics_set_pixel(graphics_handle_t handle, int16_t x, int16_t y,
                             uint16_t color)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pixel(handle, x, y, color);
    return ESP_OK;
}

esp_err_t graphics_draw_line(graphics_handle_t handle, int16_t x0, int16_t y0,
                             int16_t x1, int16_t y1, uint16_t color)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    line(handle, x0, y0, x1, y1, color);
    return ESP_OK;
}

esp_err_t graphics_fill_rect(graphics_handle_t handle, int16_t x, int16_t y,
                             int16_t width, int16_t height, uint16_t color)
{
    if (handle == NULL || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_rect(handle, x, y, width, height, color);
    return ESP_OK;
}

esp_err_t graphics_fill_circle(graphics_handle_t handle, int16_t center_x,
                               int16_t center_y, int16_t radius,
                               uint16_t color)
{
    if (handle == NULL || radius < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_circle(handle, center_x, center_y, radius, color);
    return ESP_OK;
}

static void arc(graphics_handle_t handle, int16_t center_x, int16_t center_y,
                int16_t radius, int16_t start_degrees, int16_t end_degrees,
                uint16_t color, int16_t width)
{
    int16_t last_x = center_x + (int16_t)(cosf(start_degrees * 0.0174532925f) * radius);
    int16_t last_y = center_y + (int16_t)(sinf(start_degrees * 0.0174532925f) * radius);
    for (int16_t degrees = start_degrees + 4; degrees <= end_degrees; degrees += 4) {
        int16_t x = center_x + (int16_t)(cosf(degrees * 0.0174532925f) * radius);
        int16_t y = center_y + (int16_t)(sinf(degrees * 0.0174532925f) * radius);
        for (int16_t offset = 0; offset < width; ++offset) {
            line(handle, last_x, last_y + offset, x, y + offset, color);
        }
        last_x = x;
        last_y = y;
    }
}

static const uint8_t *glyph(char character)
{
    static const uint8_t blank[] = {0, 0, 0, 0, 0};
    static const uint8_t glyphs[][6] = {
        {'0', 0x3E, 0x51, 0x49, 0x45, 0x3E},
        {'1', 0x00, 0x42, 0x7F, 0x40, 0x00},
        {'2', 0x42, 0x61, 0x51, 0x49, 0x46},
        {'3', 0x21, 0x41, 0x45, 0x4B, 0x31},
        {'4', 0x18, 0x14, 0x12, 0x7F, 0x10},
        {'5', 0x27, 0x45, 0x45, 0x45, 0x39},
        {'6', 0x3E, 0x49, 0x49, 0x49, 0x30},
        {'7', 0x01, 0x71, 0x09, 0x05, 0x03},
        {'8', 0x36, 0x49, 0x49, 0x49, 0x36},
        {'9', 0x06, 0x49, 0x49, 0x29, 0x1E},
        {'A', 0x7E, 0x11, 0x11, 0x11, 0x7E},
        {'B', 0x7F, 0x49, 0x49, 0x49, 0x36},
        {'C', 0x3E, 0x41, 0x41, 0x41, 0x22},
        {'D', 0x7F, 0x41, 0x41, 0x22, 0x1C},
        {'E', 0x7F, 0x49, 0x49, 0x49, 0x41},
        {'F', 0x7F, 0x09, 0x09, 0x09, 0x01},
        {'G', 0x3E, 0x41, 0x49, 0x49, 0x7A},
        {'H', 0x7F, 0x08, 0x08, 0x08, 0x7F},
        {'I', 0x00, 0x41, 0x7F, 0x41, 0x00},
        {'L', 0x7F, 0x40, 0x40, 0x40, 0x40},
        {'M', 0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {'N', 0x7F, 0x06, 0x18, 0x60, 0x7F},
        {'O', 0x3E, 0x41, 0x41, 0x41, 0x3E},
        {'P', 0x7F, 0x09, 0x09, 0x09, 0x06},
        {'R', 0x7F, 0x09, 0x19, 0x29, 0x46},
        {'S', 0x46, 0x49, 0x49, 0x49, 0x31},
        {'T', 0x01, 0x01, 0x7F, 0x01, 0x01},
        {'U', 0x3F, 0x40, 0x40, 0x40, 0x3F},
        {'W', 0x7F, 0x20, 0x18, 0x20, 0x7F},
        {'X', 0x63, 0x14, 0x08, 0x14, 0x63},
        {'Y', 0x07, 0x08, 0x70, 0x08, 0x07},
        {'-', 0x08, 0x08, 0x08, 0x08, 0x08},
        {':', 0x00, 0x36, 0x36, 0x00, 0x00},
        {'%', 0x63, 0x13, 0x08, 0x64, 0x63},
    };
    if (character == ' ') {
        return blank;
    }
    for (size_t index = 0; index < sizeof(glyphs) / sizeof(glyphs[0]); ++index) {
        if (glyphs[index][0] == (uint8_t)character) {
            return &glyphs[index][1];
        }
    }
    return blank;
}

static void text(graphics_handle_t handle, const char *value, int16_t center_x,
                 int16_t top_y, uint8_t scale, uint16_t color)
{
    const size_t length = strlen(value);
    const int16_t char_width = (int16_t)(6 * scale);
    const int16_t start_x = center_x - (int16_t)(length * char_width) / 2;
    for (size_t character_index = 0; character_index < length; ++character_index) {
        const uint8_t *bitmap = glyph(value[character_index]);
        int16_t x = start_x + (int16_t)character_index * char_width;
        for (int16_t column = 0; column < 5; ++column) {
            for (int16_t row = 0; row < 7; ++row) {
                if (bitmap[column] & (1 << row)) {
                    fill_rect(handle, x + column * scale, top_y + row * scale,
                              scale, scale, color);
                }
            }
        }
    }
}

static void row_text(uint8_t *row, uint16_t width, uint16_t row_y,
                     int16_t top_y, const char *value, int16_t center_x,
                     uint16_t color)
{
    const size_t length = strlen(value);
    const int16_t start_x = center_x - (int16_t)(length * 6) / 2;
    if (top_y < 0 || row_y < (uint16_t)top_y || row_y >= (uint16_t)(top_y + 7)) {
        return;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t *bitmap = glyph(value[index]);
        const int16_t x = start_x + (int16_t)index * 6;
        for (int16_t column = 0; column < 5; ++column) {
            const int16_t pixel_x = x + column;
            if (pixel_x < 0 || pixel_x >= (int16_t)width) {
                continue;
            }
            for (int16_t bit = 0; bit < 7; ++bit) {
                if (top_y + bit == (int16_t)row_y && (bitmap[column] & (1 << bit))) {
                    row[pixel_x * 2] = (uint8_t)(color >> 8);
                    row[pixel_x * 2 + 1] = (uint8_t)color;
                }
            }
        }
    }
}

esp_err_t graphics_draw_text(graphics_handle_t handle, const char *value,
                             int16_t center_x, int16_t top_y, uint8_t scale,
                             uint16_t color)
{
    if (handle == NULL || value == NULL || scale == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    text(handle, value, center_x, top_y, scale, color);
    return ESP_OK;
}

static void draw_startup_robot(graphics_handle_t handle, uint8_t phase)
{
    const int16_t x = 87;
    const int16_t y = 61;
    const int16_t cell = 7;
    const uint16_t accent = phase >= 3 ? GRAPHICS_GREEN : GRAPHICS_CYAN;

    fill_rect(handle, x + 3 * cell, y - 3 * cell, cell, cell, accent);
    fill_rect(handle, x + 2 * cell, y - 2 * cell, 3 * cell, cell, accent);
    fill_rect(handle, x - cell, y + cell, cell, 5 * cell, accent);
    fill_rect(handle, x + 8 * cell, y + cell, cell, 5 * cell, accent);
    fill_rect(handle, x, y, 7 * cell, 6 * cell, accent);
    fill_rect(handle, x + cell, y + cell, 5 * cell, 4 * cell, GRAPHICS_BLACK);

    const uint16_t eye = phase & 1 ? GRAPHICS_YELLOW : GRAPHICS_WHITE;
    fill_rect(handle, x + 2 * cell, y + 2 * cell, cell, cell, eye);
    fill_rect(handle, x + 4 * cell, y + 2 * cell, cell, cell, eye);
    fill_rect(handle, x + 2 * cell, y + 4 * cell, 3 * cell, cell,
              phase >= 4 ? GRAPHICS_GREEN : GRAPHICS_WHITE);
    if (phase & 1) {
        fill_rect(handle, x + 3 * cell, y + 3 * cell, cell, cell,
                  GRAPHICS_BLACK);
    }
}

esp_err_t graphics_draw_startup_screen(graphics_handle_t handle, uint8_t phase)
{
    if (handle == NULL || handle->width != 240 || handle->height != 240) {
        return ESP_ERR_INVALID_ARG;
    }

    if (phase > 4) {
        phase = 4;
    }
    esp_err_t error = graphics_clear(handle, GRAPHICS_BLACK);
    if (error != ESP_OK) {
        return error;
    }

    for (int16_t y = 28; y < 216; y += 12) {
        for (int16_t x = 28; x < 216; x += 12) {
            fill_rect(handle, x, y, 2, 2, GRAPHICS_DIM_BLUE);
        }
    }
    fill_rect(handle, 35, 21, 170, 2, GRAPHICS_CYAN);
    fill_rect(handle, 35, 217, 170, 2, GRAPHICS_DIM_BLUE);
    text(handle, "BINBOT", 120, 29, 3, GRAPHICS_WHITE);
    text(handle, "PIXEL OS", 120, 132, 2, GRAPHICS_CYAN);
    draw_startup_robot(handle, phase);

    static const char *const statuses[] = {
        "BOOT", "SCAN", "LOAD", "GIF", "READY"
    };
    text(handle, statuses[phase], 120, 157, 2,
         phase >= 4 ? GRAPHICS_GREEN : GRAPHICS_YELLOW);

    fill_rect(handle, 52, 190, 136, 8, GRAPHICS_DIM_BLUE);
    const int16_t progress_width = (int16_t)(phase * 34);
    if (progress_width > 0) {
        fill_rect(handle, 52, 190, progress_width, 8,
                  phase >= 4 ? GRAPHICS_GREEN : GRAPHICS_CYAN);
    }
    return ESP_OK;
}

static void draw_mario_sprite(graphics_handle_t handle, uint8_t frame)
{
    static const char *const sprite[] = {
        "......RRRR......",
        "....RRRRRRRR....",
        "...RRRRRRRRRR...",
        "...SSSDSSSS.....",
        "..SSSSSSSSSS....",
        "..SSKSSSSKSS....",
        "..SSSSSSSSSS....",
        "...SSSSSSSS.....",
        "....YYYY........",
        "..RRBBBBBBRR....",
        ".RRRBBBBBBRRR...",
        ".RRRBBBBBBRRR...",
        "...BBBBBBBB.....",
        "...BBBBBBBB.....",
        "..BB..BBBB......",
        "..BB..BBBB......"
    };
    const int16_t x = 80;
    const int16_t y = 57;
    const int16_t cell = 5;
    for (size_t row = 0; row < sizeof(sprite) / sizeof(sprite[0]); ++row) {
        for (size_t column = 0; sprite[row][column] != '\0'; ++column) {
            uint16_t color = GRAPHICS_BLACK;
            switch (sprite[row][column]) {
            case 'R': color = GRAPHICS_RED; break;
            case 'S': color = 0xFD20; break;
            case 'K': color = GRAPHICS_BLACK; break;
            case 'B': color = 0x001F; break;
            case 'Y': color = GRAPHICS_YELLOW; break;
            default: continue;
            }
            fill_rect(handle, x + (int16_t)column * cell,
                      y + (int16_t)row * cell, cell, cell, color);
        }
    }

    const int16_t foot_y = y + 16 * cell;
    if (frame & 1) {
        fill_rect(handle, x + 1 * cell, foot_y, 4 * cell, cell, 0xA145);
        fill_rect(handle, x + 7 * cell, foot_y, 4 * cell, cell, 0xA145);
    } else {
        fill_rect(handle, x + 2 * cell, foot_y, 4 * cell, cell, 0xA145);
        fill_rect(handle, x + 8 * cell, foot_y, 4 * cell, cell, 0xA145);
    }
}

esp_err_t graphics_draw_mario_screen(graphics_handle_t handle, uint8_t phase)
{
    if (handle == NULL || handle->width != 240 || handle->height != 240) {
        return ESP_ERR_INVALID_ARG;
    }
    if (phase > 4) {
        phase = 4;
    }

    const uint16_t sky = 0x35BF;
    const uint16_t brick = 0xA145;
    const uint16_t ground = 0x79E0;
    esp_err_t error = graphics_clear(handle, sky);
    if (error != ESP_OK) {
        return error;
    }

    // Keep the scene pixelated: every shape is aligned to a 5-pixel grid.
    fill_rect(handle, 24, 51, 35, 5, GRAPHICS_WHITE);
    fill_rect(handle, 34, 46, 20, 5, GRAPHICS_WHITE);
    fill_rect(handle, 178, 57, 38, 5, GRAPHICS_WHITE);
    fill_rect(handle, 188, 52, 18, 5, GRAPHICS_WHITE);

    text(handle, "MARIO", 45, 10, 1, GRAPHICS_WHITE);
    text(handle, "WORLD 1-1", 120, 10, 1, GRAPHICS_WHITE);
    text(handle, "SCORE 04200", 60, 25, 1, GRAPHICS_WHITE);
    text(handle, "COIN 02", 186, 25, 1, GRAPHICS_YELLOW);

    fill_rect(handle, 42, 78, 25, 25, GRAPHICS_YELLOW);
    fill_rect(handle, 47, 83, 15, 15, 0xF800);
    fill_rect(handle, 173, 84, 25, 25, GRAPHICS_YELLOW);
    fill_rect(handle, 178, 89, 15, 15, 0xF800);
    draw_mario_sprite(handle, phase);

    fill_rect(handle, 0, 192, 240, 8, ground);
    for (int16_t x = 0; x < 240; x += 20) {
        fill_rect(handle, x, 200, 18, 40, brick);
        fill_rect(handle, x + 2, 202, 16, 3, 0xC5A0);
    }
    for (int16_t x = 0; x < 240; x += 40) {
        fill_rect(handle, x + 18, 210, 4, 4, 0x5A20);
    }

    fill_rect(handle, 81, 170, 78, 13, 0x001F);
    text(handle, "READY", 120, 173, 1, GRAPHICS_GREEN);
    return ESP_OK;
}

esp_err_t graphics_init(uint16_t width, uint16_t height,
                        graphics_handle_t *handle)
{
    if (handle == NULL || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;
    graphics_handle_t context = heap_caps_calloc(1, sizeof(*context), MALLOC_CAP_INTERNAL);
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    context->width = width;
    context->height = height;
    context->frame_bytes = (size_t)width * height * 2;
    // Display driver stages ordinary RAM into a small DMA buffer when needed.
    context->frame = heap_caps_malloc(context->frame_bytes,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (context->frame == NULL) {
        heap_caps_free(context);
        return ESP_ERR_NO_MEM;
    }
    *handle = context;
    return graphics_clear(context, GRAPHICS_BLACK);
}

esp_err_t graphics_get_info(graphics_handle_t handle, uint16_t *width,
                            uint16_t *height, size_t *frame_bytes)
{
    if (handle == NULL || width == NULL || height == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *width = handle->width;
    *height = handle->height;
    *frame_bytes = handle->frame_bytes;
    return ESP_OK;
}

esp_err_t graphics_clear(graphics_handle_t handle, uint16_t color)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < (size_t)handle->width * handle->height; ++index) {
        handle->frame[index * 2] = (uint8_t)(color >> 8);
        handle->frame[index * 2 + 1] = (uint8_t)color;
    }
    return ESP_OK;
}

esp_err_t graphics_fill_color(graphics_handle_t handle, uint16_t color)
{
    return graphics_clear(handle, color);
}

esp_err_t graphics_draw_ota_progress(graphics_handle_t handle, uint8_t percent,
                                     bool complete)
{
    if (handle == NULL || handle->width == 0 || handle->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    const int16_t center_x = (int16_t)(handle->width / 2);
    const int16_t bar_width = handle->width >= 300 ? 240 : 188;
    const int16_t bar_x = center_x - bar_width / 2;
    const int16_t bar_y = (int16_t)(handle->height / 2 - 6);
    char percent_text[8];

    esp_err_t error = graphics_clear(handle, GRAPHICS_BLACK);
    if (error != ESP_OK) {
        return error;
    }
    text(handle, complete ? "UPDATED" : "UPDATING", center_x, 58, 2,
         complete ? GRAPHICS_GREEN : GRAPHICS_CYAN);
    fill_round_rect(handle, bar_x, bar_y, bar_width, 16, 8,
                    GRAPHICS_DIM_BLUE);
    if (percent > 0) {
        const int16_t fill_width = (int16_t)(((bar_width - 6) * percent) / 100);
        fill_round_rect(handle, bar_x + 3, bar_y + 3, fill_width, 10, 5,
                        complete ? GRAPHICS_GREEN : GRAPHICS_CYAN);
    }
    (void)snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
    text(handle, percent_text, center_x, bar_y + 28, 2, GRAPHICS_WHITE);
    return ESP_OK;
}

static bool row_round_rect_contains(int16_t px, int16_t py, int16_t x,
                                    int16_t y, int16_t width, int16_t height,
                                    int16_t radius)
{
    if (px < x || px >= x + width || py < y || py >= y + height) {
        return false;
    }
    if (radius <= 0) {
        return true;
    }
    int16_t cx = px;
    int16_t cy = py;
    if (px < x + radius) {
        cx = x + radius - 1;
    } else if (px >= x + width - radius) {
        cx = x + width - radius;
    } else {
        return true;
    }
    if (py < y + radius) {
        cy = y + radius - 1;
    } else if (py >= y + height - radius) {
        cy = y + height - radius;
    } else {
        return true;
    }
    const int16_t dx = px - cx;
    const int16_t dy = py - cy;
    const int16_t edge = radius - 1;
    return dx * dx + dy * dy <= edge * edge;
}

esp_err_t graphics_render_ota_row(uint8_t *row, size_t row_bytes,
                                  uint16_t width, uint16_t row_y,
                                  uint8_t percent, bool complete)
{
    if (row == NULL || width == 0 || row_bytes != (size_t)width * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    const int16_t bar_y = 112;
    const int16_t bar_height = 16;
    const int16_t bar_x = width >= 240 ? 26 : 0;
    const int16_t bar_width = width >= 240 ? 188 : (int16_t)width;
    const int16_t radius = bar_height / 2;
    const int16_t fill_width = (int16_t)(((bar_width - 6) * percent) / 100);
    for (uint16_t x = 0; x < width; ++x) {
        uint16_t color = GRAPHICS_BLACK;
        const int16_t pixel_x = (int16_t)x;
        const int16_t pixel_y = (int16_t)row_y;
        if (row_round_rect_contains(pixel_x, pixel_y, bar_x, bar_y,
                                    bar_width, bar_height, radius)) {
            color = GRAPHICS_DIM_BLUE;
            if (fill_width > 0 && pixel_x < bar_x + 3 + fill_width &&
                row_round_rect_contains(pixel_x, pixel_y, bar_x + 3,
                                        bar_y + 3, fill_width, bar_height - 6,
                                        (bar_height - 6) / 2)) {
                color = complete ? GRAPHICS_GREEN : GRAPHICS_CYAN;
            }
        }
        row[x * 2] = (uint8_t)(color >> 8);
        row[x * 2 + 1] = (uint8_t)color;
    }
    const int16_t center_x = (int16_t)(width / 2);
    if (row_y >= 103 && row_y <= 109) {
        row_text(row, width, row_y, 103, complete ? "UPDATED" : "UPDATING",
                 center_x, complete ? GRAPHICS_GREEN : GRAPHICS_CYAN);
    }
    if (row_y >= 132 && row_y <= 138) {
        char percent_text[8];
        (void)snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
        row_text(row, width, row_y, 132, percent_text, center_x, GRAPHICS_WHITE);
    }
    return ESP_OK;
}

esp_err_t graphics_draw_face(graphics_handle_t handle, uint8_t emotion,
                             bool soft_style)
{
    if (handle == NULL || handle->width != 240 || handle->height != 240) {
        return ESP_ERR_INVALID_ARG;
    }
    const int16_t center = 120;
    esp_err_t error = graphics_clear(handle, GRAPHICS_BLACK);
    if (error != ESP_OK) {
        return error;
    }
    if (emotion == 0) {
        draw_reference_anger_face(handle);
        return ESP_OK;
    }
    if (soft_style) {
        fill_round_rect(handle, 56, 92, 42, 31, 15, GRAPHICS_WHITE);
        fill_round_rect(handle, 142, 92, 42, 31, 15, GRAPHICS_WHITE);
    } else {
        fill_circle(handle, 77, 107, 18, GRAPHICS_WHITE);
        fill_circle(handle, 163, 107, 18, GRAPHICS_WHITE);
    }
    switch (emotion) {
    case 0:
        arc(handle, center, 143, 43, 18, 162, GRAPHICS_WHITE, 4);
        fill_circle(handle, 77, 107, 6, GRAPHICS_BLACK);
        fill_circle(handle, 163, 107, 6, GRAPHICS_BLACK);
        break;
    case 1:
        line(handle, 54, 88, 96, 101, GRAPHICS_BLACK);
        line(handle, 144, 101, 186, 88, GRAPHICS_BLACK);
        fill_round_rect(handle, 93, 151, 54, 13, 6, GRAPHICS_WHITE);
        break;
    case 2:
        fill_circle(handle, 77, 107, 6, GRAPHICS_BLACK);
        fill_circle(handle, 163, 107, 6, GRAPHICS_BLACK);
        arc(handle, center, 180, 42, 198, 342, GRAPHICS_WHITE, 4);
        break;
    default:
        line(handle, 59, 108, 92, 99, GRAPHICS_WHITE);
        line(handle, 148, 99, 181, 108, GRAPHICS_WHITE);
        fill_round_rect(handle, 103, 143, 34, 29, 14, GRAPHICS_WHITE);
        fill_round_rect(handle, 111, 148, 18, 15, 7, GRAPHICS_BLACK);
        break;
    }
    return ESP_OK;
}

esp_err_t graphics_draw_face_animation(graphics_handle_t handle,
                                        uint8_t emotion, bool soft_style,
                                        uint8_t animation_frame)
{
    if (handle == NULL || handle->width != 240 || handle->height != 240) {
        return ESP_ERR_INVALID_ARG;
    }
    if (emotion != 0) {
        return graphics_draw_face(handle, emotion, soft_style);
    }

    esp_err_t error = graphics_clear(handle, GRAPHICS_BLACK);
    if (error != ESP_OK) {
        return error;
    }
    draw_reference_anger_blink(handle, animation_frame % 4);
    return ESP_OK;
}

static void draw_cloud(graphics_handle_t handle)
{
    fill_circle(handle, 120, 91, 18, GRAPHICS_WHITE);
    fill_circle(handle, 99, 101, 14, GRAPHICS_WHITE);
    fill_circle(handle, 141, 101, 14, GRAPHICS_WHITE);
    fill_round_rect(handle, 94, 98, 52, 20, 10, GRAPHICS_WHITE);
}

esp_err_t graphics_draw_weather(graphics_handle_t handle)
{
    if (handle == NULL || handle->width != 240 || handle->height != 240) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = graphics_clear(handle, GRAPHICS_BLACK);
    if (error != ESP_OK) {
        return error;
    }
    text(handle, "12:48", 120, 30, 2, GRAPHICS_WHITE);
    draw_cloud(handle);
    text(handle, "22 C", 120, 127, 3, GRAPHICS_WHITE);
    text(handle, "SHANGHAI", 120, 174, 1, GRAPHICS_BLUE);
    fill_rect(handle, 62, 199, 116, 2, GRAPHICS_BLUE);
    text(handle, "CLEAR", 120, 207, 1, GRAPHICS_WHITE);
    return ESP_OK;
}

esp_err_t graphics_get_frame(graphics_handle_t handle, const uint8_t **frame,
                             size_t *frame_bytes)
{
    if (handle == NULL || frame == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame = handle->frame;
    *frame_bytes = handle->frame_bytes;
    return ESP_OK;
}

esp_err_t graphics_get_frame_buffer(graphics_handle_t handle, uint8_t **frame,
                                    size_t *frame_bytes)
{
    if (handle == NULL || frame == NULL || frame_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame = handle->frame;
    *frame_bytes = handle->frame_bytes;
    return ESP_OK;
}
