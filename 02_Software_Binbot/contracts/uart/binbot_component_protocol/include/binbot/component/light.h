#ifndef BINBOT_COMPONENT_LIGHT_H
#define BINBOT_COMPONENT_LIGHT_H

#include "binbot/component/protocol.h"

#define BB_COMPONENT_LIGHT_BYTES 9
#define BB_COMPONENT_LIGHT_STATE_BYTES 11

enum bb_component_light_effect {
    BB_COMPONENT_LIGHT_SOLID, BB_COMPONENT_LIGHT_BREATHE, BB_COMPONENT_LIGHT_FLOW,
    BB_COMPONENT_LIGHT_BLINK, BB_COMPONENT_LIGHT_BEAT
};

typedef struct {
    uint8_t enabled, effect, red, green, blue, brightness, speed;
    uint16_t timer_minutes;
} bb_component_light_settings_t;

typedef struct {
    bb_component_light_settings_t settings;
    uint64_t started_ms, deadline_ms;
} bb_component_light_t;

bool bb_component_light_valid(const bb_component_light_settings_t *settings);
bool bb_component_light_decode(const uint8_t *bytes, size_t length,
                                bb_component_light_settings_t *settings);
void bb_component_light_encode(const bb_component_light_settings_t *settings, uint8_t *bytes);
bool bb_component_light_set(bb_component_light_t *light,
                             const bb_component_light_settings_t *settings, uint64_t now_ms);
uint16_t bb_component_light_remaining(const bb_component_light_t *light, uint64_t now_ms);
/* Returns true when the timer switches the light off. Pixels are scaled RGB. */
bool bb_component_light_render(bb_component_light_t *light, uint64_t now_ms,
                                uint8_t *pixels, size_t led_count);

#endif
