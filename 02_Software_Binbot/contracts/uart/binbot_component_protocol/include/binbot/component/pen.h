#ifndef BINBOT_COMPONENT_PEN_H
#define BINBOT_COMPONENT_PEN_H
#include "binbot/component/protocol.h"

#define BB_COMPONENT_PEN_BYTES 24
#define BB_PEN_TIME_VALID 1
#define BB_PEN_WEATHER_VALID 2
enum { BB_PEN_IMAGE, BB_PEN_INFO };
enum { BB_PEN_BOTH, BB_PEN_TIME, BB_PEN_WEATHER };
enum { BB_PEN_SUN, BB_PEN_CLOUD, BB_PEN_RAIN, BB_PEN_SNOW, BB_PEN_STORM, BB_PEN_FOG,
       BB_PEN_OVERCAST, BB_PEN_NIGHT, BB_PEN_CLOUD_NIGHT, BB_PEN_SHOWER,
       BB_PEN_HEAVY_RAIN, BB_PEN_DOWNPOUR, BB_PEN_SLEET, BB_PEN_HEAVY_SNOW,
       BB_PEN_HAIL, BB_PEN_WIND, BB_PEN_HAZE, BB_PEN_DUST, BB_PEN_TYPHOON,
       BB_PEN_HOT, BB_PEN_CONDITION_COUNT };
typedef struct {
    uint8_t mode, face, flags, condition;
    uint32_t unix_time;
    int32_t utc_offset;
    int16_t temperature, low, high; /* Tenths of a degree Celsius. */
    uint32_t weather_time;
} bb_component_pen_t;

bool bb_component_pen_valid(const bb_component_pen_t *state);
bool bb_component_pen_decode(const uint8_t *bytes, size_t length, bb_component_pen_t *state);
void bb_component_pen_encode(const bb_component_pen_t *state, uint8_t *bytes);
void bb_component_pen_advance(bb_component_pen_t *state, uint32_t seconds);
/* A Figma face at 240 x 240, RGB565 in display wire byte order. */
void bb_component_pen_render(const bb_component_pen_t *state, uint8_t *pixels);
void bb_component_pen_render_face(const bb_component_pen_t *state, uint8_t *pixels, bool detail);
#endif
