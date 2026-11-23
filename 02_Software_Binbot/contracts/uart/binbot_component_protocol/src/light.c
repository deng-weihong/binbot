#include "binbot/component/light.h"

bool bb_component_light_valid(const bb_component_light_settings_t *s)
{
    return s && s->enabled <= 1 && s->effect <= BB_COMPONENT_LIGHT_BEAT &&
           s->brightness <= 100 && s->speed >= 5 && s->speed <= 100 &&
           s->timer_minutes <= 120;
}

bool bb_component_light_decode(const uint8_t *b, size_t n, bb_component_light_settings_t *s)
{
    if(!b || !s || n != BB_COMPONENT_LIGHT_BYTES) return false;
    *s = (bb_component_light_settings_t){b[0], b[1], b[2], b[3], b[4], b[5], b[6],
                                        bb_component_get_u16(b + 7)};
    return bb_component_light_valid(s);
}

void bb_component_light_encode(const bb_component_light_settings_t *s, uint8_t *b)
{
    b[0] = s->enabled; b[1] = s->effect; b[2] = s->red; b[3] = s->green;
    b[4] = s->blue; b[5] = s->brightness; b[6] = s->speed;
    bb_component_put_u16(b + 7, s->timer_minutes);
}

bool bb_component_light_set(bb_component_light_t *l, const bb_component_light_settings_t *s,
                             uint64_t now)
{
    if(!l || !bb_component_light_valid(s)) return false;
    /* Color/brightness changes retain the running timer; a new timer restarts it. */
    if(s->enabled && s->timer_minutes &&
       (!l->settings.enabled || s->timer_minutes != l->settings.timer_minutes || !l->deadline_ms))
        l->deadline_ms = now + (uint64_t)s->timer_minutes * 60000;
    if(!s->enabled || !s->timer_minutes) l->deadline_ms = 0;
    if(!l->settings.enabled || s->effect != l->settings.effect || s->speed != l->settings.speed)
        l->started_ms = now;
    l->settings = *s;
    return true;
}

uint16_t bb_component_light_remaining(const bb_component_light_t *l, uint64_t now)
{
    return l->deadline_ms > now ? (uint16_t)((l->deadline_ms - now + 59999) / 60000) : 0;
}

bool bb_component_light_render(bb_component_light_t *l, uint64_t now, uint8_t *pixels, size_t count)
{
    bool expired = l->deadline_ms && now >= l->deadline_ms;
    if(expired) { l->settings.enabled = 0; l->settings.timer_minutes = 0; l->deadline_ms = 0; }
    const bb_component_light_settings_t *s = &l->settings;
    unsigned period = (s->effect == BB_COMPONENT_LIGHT_FLOW ? 9000 :
                       s->effect == BB_COMPONENT_LIGHT_BEAT ? 1400 : 4000) * (150 - s->speed) / 100;
    if(!period) period = 1;
    unsigned phase = (unsigned)(((now - l->started_ms) % period) * 1000 / period);
    unsigned level = s->enabled ? s->brightness : 0;
    if(s->effect == BB_COMPONENT_LIGHT_BREATHE) {
        unsigned ramp = phase <= 500 ? phase * 2 : (1000 - phase) * 2;
        ramp = ramp * ramp * (3000 - 2 * ramp) / 1000000;
        level = level * (250 + ramp * 3 / 4) / 1000;
    } else if(s->effect == BB_COMPONENT_LIGHT_BLINK) {
        level = level * (phase < 500 ? 100 : 15) / 100;
    } else if(s->effect == BB_COMPONENT_LIGHT_BEAT) {
        unsigned pulse = phase < 150 ? phase * 1000 / 150 : phase < 300 ? (300 - phase) * 1000 / 150 :
                         phase < 450 ? (phase - 300) * 750 / 150 : phase < 600 ? (600 - phase) * 750 / 150 : 0;
        level = level * (200 + pulse * 4 / 5) / 1000;
    }
    for(size_t i = 0; i < count; ++i) {
        unsigned r = s->red, g = s->green, b = s->blue;
        if(s->effect == BB_COMPONENT_LIGHT_FLOW) {
            unsigned maximum = r > g ? r : g; if(b > maximum) maximum = b;
            unsigned minimum = r < g ? r : g; if(b < minimum) minimum = b;
            int base = 0, difference = (int)maximum - minimum;
            if(difference) {
                base = maximum == r ? ((int)g - (int)b) * 256 / difference :
                       maximum == g ? 512 + ((int)b - (int)r) * 256 / difference :
                                      1024 + ((int)r - (int)g) * 256 / difference;
            }
            unsigned hue = (base + 1536 + phase * 1536 / 1000 + (count ? i * 1536 / count : 0)) % 1536;
            unsigned x = hue % 256;
            switch(hue / 256) {
            case 0: r = 255; g = x; b = 0; break;
            case 1: r = 255 - x; g = 255; b = 0; break;
            case 2: r = 0; g = 255; b = x; break;
            case 3: r = 0; g = 255 - x; b = 255; break;
            case 4: r = x; g = 0; b = 255; break;
            default: r = 255; g = 0; b = 255 - x; break;
            }
            r = minimum + r * difference / 255;
            g = minimum + g * difference / 255;
            b = minimum + b * difference / 255;
        }
        pixels[i * 3] = r * level / 100;
        pixels[i * 3 + 1] = g * level / 100;
        pixels[i * 3 + 2] = b * level / 100;
    }
    return expired;
}
