#include "binbot/component/pen.h"
#include <limits.h>
#include <string.h>

bool bb_component_pen_valid(const bb_component_pen_t *s)
{
    return s && s->mode <= BB_PEN_INFO && s->face <= BB_PEN_WEATHER && s->flags <= 3 &&
        s->condition < BB_PEN_CONDITION_COUNT && s->utc_offset >= -43200 && s->utc_offset <= 50400 &&
        (!(s->flags & BB_PEN_TIME_VALID) || s->unix_time >= 1577836800U) &&
        (!(s->flags & BB_PEN_WEATHER_VALID) ||
         ((s->flags & BB_PEN_TIME_VALID) && s->weather_time <= s->unix_time &&
          s->temperature >= -1000 && s->temperature <= 700 && s->low >= -1000 &&
          s->low <= s->high && s->high <= 700));
}
void bb_component_pen_encode(const bb_component_pen_t *s, uint8_t *b)
{
    memset(b, 0, BB_COMPONENT_PEN_BYTES);
    b[0] = s->mode; b[1] = s->face; b[2] = s->flags; b[3] = s->condition;
    bb_component_put_u32(b + 4, s->unix_time);
    bb_component_put_u32(b + 8, (uint32_t)s->utc_offset);
    bb_component_put_u16(b + 12, (uint16_t)s->temperature);
    bb_component_put_u16(b + 14, (uint16_t)s->low);
    bb_component_put_u16(b + 16, (uint16_t)s->high);
    bb_component_put_u32(b + 18, s->weather_time);
}
bool bb_component_pen_decode(const uint8_t *b, size_t n, bb_component_pen_t *s)
{
    if(!b || !s || n != BB_COMPONENT_PEN_BYTES || b[22] || b[23]) return false;
    bb_component_pen_t next = {.mode=b[0], .face=b[1], .flags=b[2], .condition=b[3],
        .unix_time=bb_component_get_u32(b+4), .utc_offset=(int32_t)bb_component_get_u32(b+8),
        .temperature=(int16_t)bb_component_get_u16(b+12), .low=(int16_t)bb_component_get_u16(b+14),
        .high=(int16_t)bb_component_get_u16(b+16), .weather_time=bb_component_get_u32(b+18)};
    if(!bb_component_pen_valid(&next)) return false;
    *s = next;
    return true;
}
void bb_component_pen_advance(bb_component_pen_t *s, uint32_t seconds)
{
    if(s->flags & BB_PEN_TIME_VALID) {
        if(seconds > UINT32_MAX - s->unix_time) s->flags = 0;
        else s->unix_time += seconds;
    }
    if((s->flags & BB_PEN_WEATHER_VALID) &&
       (s->weather_time > s->unix_time || s->unix_time - s->weather_time > 7200))
        s->flags &= ~BB_PEN_WEATHER_VALID;
}
