#include "binbot/component/pen.h"
#include <string.h>
#include "pen_assets.inc"

static void pixel(uint8_t *p, int x, int y, const uint8_t *rgb, unsigned alpha)
{
    if(x < 0 || y < 0 || x >= 240 || y >= 240) return;
    p += (y * 240 + x) * 2;
    unsigned old = alpha == 255 ? 0 : (unsigned)p[0] << 8 | p[1];
    unsigned r = ((old >> 11) * 255 / 31 * (255-alpha) + rgb[0]*alpha) / 255;
    unsigned g = (((old >> 5) & 63) * 255 / 63 * (255-alpha) + rgb[1]*alpha) / 255;
    unsigned b = ((old & 31) * 255 / 31 * (255-alpha) + rgb[2]*alpha) / 255;
    static const uint8_t bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    unsigned bias = bayer[(y%4)*4+x%4] * 255 / 16;
    /* Ordered dithering preserves the neutral, smooth dark Figma gradient in RGB565. */
    unsigned v = ((r*31+bias)/255) << 11 | ((g*63+bias)/255) << 5 | ((b*31+bias)/255);
    p[0] = v >> 8; p[1] = v;
}

static int glyph_index(char c)
{
    const char *chars = "0123456789:-o/. ";
    const char *found = strchr(chars, c);
    return found ? (int)(found - chars) : 11;
}

static int text_width(const char *text, int size)
{
    int width = 0;
    for(; *text; ++text) width += pen_widths[glyph_index(*text)] * size / 80;
    return width;
}

static void text(uint8_t *p, const char *value, int size, int center, int top, unsigned opacity)
{
    int x = center - text_width(value, size) / 2;
    const uint8_t white[] = {255,255,255};
    for(; *value; ++value) {
        int glyph = glyph_index(*value), width = pen_widths[glyph] * size / 80;
        for(int y = 0; y < size * 96 / 80; ++y) for(int dx = 0; dx < width; ++dx) {
            int sx = dx * 80 / size, sy = y * 80 / size;
            unsigned a = pen_glyphs[glyph * 52 * 96 + sy * 52 + sx];
            if(a) pixel(p, x + dx, top + y, white, a * opacity / 255);
        }
        x += width;
    }
}

static void weather_icon(uint8_t *p, unsigned condition, int center, int top, int diameter)
{
    if(condition >= BB_PEN_CONDITION_COUNT) return;
    int width = (diameter * 4 + 2) / 3 + 40, height = diameter + 40;
    int left = center - (width-40)/2 - 20;
    const uint8_t *source = diameter == 70 ? pen_weather_70 + pen_weather_70_offsets[condition] :
        pen_weather_38 + pen_weather_38_offsets[condition];
    /* Lossless RGBA runs retain the Figma clouds and soft, fixed-size glow. */
    for(int offset = 0; offset < width*height;) {
        unsigned count = source[0], alpha = source[4];
        if(alpha) for(unsigned i=0;i<count;++i) {
            int index = offset + i;
            pixel(p, left+index%width, top-20+index/width, source+1, alpha);
        }
        offset += count; source += 5;
    }
}

static char *number(char *out, int value)
{
    if(value < 0) { *out++ = '-'; value = -value; }
    if(value >= 100) *out++ = '0' + value/100;
    if(value >= 10) *out++ = '0' + value/10%10;
    *out++ = '0' + value%10;
    return out;
}

static char *degrees(char *out, int tenths)
{
    out = number(out, (tenths + (tenths < 0 ? -5 : 5))/10);
    *out++ = 'o'; *out = 0;
    return out;
}

void bb_component_pen_render_face(const bb_component_pen_t *s, uint8_t *p, bool detail)
{
    const uint8_t *background = (s->flags & BB_PEN_WEATHER_VALID) &&
        (s->condition == BB_PEN_NIGHT || s->condition == BB_PEN_CLOUD_NIGHT) ?
        pen_night_background : pen_background;
    for(int y = 0; y < 240; ++y) for(int x = 0; x < 240; ++x)
        pixel(p, x, y, background + ((y*100/240)*100+x*100/240)*3, 255);
    char hm[8] = "--:--", temp[16] = "--o", range[32];
    if(s->flags & BB_PEN_TIME_VALID) {
        int64_t local = (int64_t)s->unix_time + s->utc_offset;
        unsigned minutes = (unsigned)((local / 60) % 1440);
        hm[0] = '0' + minutes/600; hm[1] = '0' + minutes/60%10;
        hm[3] = '0' + minutes%60/10; hm[4] = '0' + minutes%10;
    }
    bool weather = s->flags & BB_PEN_WEATHER_VALID;
    if(weather) degrees(temp, s->temperature);
    if(s->face == BB_PEN_TIME) text(p, hm, 77, 120, 70, 255);
    else if(s->face == BB_PEN_BOTH) {
        text(p, hm, 66, 120, 53, 255);
        int tw = text_width(temp, 38), group = 52 + 10 + tw;
        int start = (240-group)/2;
        if(weather) weather_icon(p, s->condition, start+26, 142, 38);
        text(p, temp, 38, weather ? start+62+tw/2 : 120, 136, 191);
    } else {
        if(weather) weather_icon(p, s->condition, 120, 48, 70);
        text(p, temp, 70, 120, 109, 255);
        if(weather && detail) {
            memcpy(range, " . ", 3);
            char *next = degrees(range+3, s->low); *next++ = '/'; degrees(next, s->high);
            int cw = pen_condition_widths[s->condition], rw = text_width(range, 25);
            int left = (240-cw-rw)/2;
            const uint8_t white[] = {255,255,255};
            for(int y=0;y<36;++y) for(int x=0;x<cw;++x) {
                unsigned a=pen_conditions[s->condition*80*36+y*80+x];
                if(a) pixel(p, left+x, 192+y, white, a/2);
            }
            text(p, range, 25, left+cw+rw/2, 192, 128);
        }
    }
}

void bb_component_pen_render(const bb_component_pen_t *s, uint8_t *p)
{
    bb_component_pen_render_face(s, p, true);
}
