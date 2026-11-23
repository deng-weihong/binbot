#include "../ambient_internal.h"
#include "face_frames.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MASK_X = 120, MASK_Y = 216, MASK_WIDTH = 480, MASK_HEIGHT = 288 };

typedef struct {
    binbot_ambient_display_t *display;
    binbot_ambient_snapshot_t snapshot;
    lv_obj_t *root, *eyes, *clock, *effects[4];
    FT_Library rasterizer;
    uint8_t *pixels;
    lv_image_dsc_t mask;
    saver_point_t from[2][64];
    float from_hole;
    float opacity_from[4];
    float opacity[4];
    uint32_t started_at, frame_at;
    unsigned face, frame;
    char time_text[64];
} face_t;

static const struct {
    const char *name;
    int x, y, width, height;
} effects[] = {
    {"blush", 168, 434, 341, 53},
    {"spark", 556, 160, 96, 96},
    {"anger", 517, 114, 118, 118},
    {"zzz", 492, 151, 151, 188},
};

static float mix(float a, float b, float k) { return a + (b - a) * k; }

static float morph_ease(float t)
{
    return t < .5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}

static float bezier(float t, float a, float b)
{
    return 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t;
}

static float css_ease(float t, bool opacity)
{
    float lo = 0, hi = 1;
    for(unsigned i = 0; i < 12; ++i) {
        float mid = (lo + hi) / 2;
        if(bezier(mid, opacity ? .25f : .42f, opacity ? .25f : .58f) < t) lo = mid;
        else hi = mid;
    }
    return bezier((lo + hi) / 2, opacity ? .1f : 0, 1);
}

static const saver_frame_t *current_frame(const face_t *saver)
{
    return &saver_faces[saver->face].frames[saver->frame];
}

static void pose(face_t *saver, uint32_t elapsed, saver_point_t eye[2][64], float *hole)
{
    const saver_frame_t *frame = current_frame(saver);
    float k = morph_ease(fminf(1, (float)elapsed / frame->morph));
    for(unsigned side = 0; side < 2; ++side) for(unsigned i = 0; i < 64; ++i) {
        eye[side][i].x = mix(saver->from[side][i].x, frame->eye[side][i].x, k);
        eye[side][i].y = mix(saver->from[side][i].y, frame->eye[side][i].y, k);
    }
    *hole = mix(saver->from_hole, frame->hole, k);
}

static void render_eyes(face_t *saver, uint32_t elapsed, float dx, float dy)
{
    saver_point_t eye[2][64];
    float hole;
    pose(saver, elapsed, eye, &hole);
    FT_Vector points[256];
    char tags[256];
    short contours[4];
    unsigned count = 0, contour_count = 0;
    for(unsigned side = 0; side < 2; ++side) {
        for(unsigned i = 0; i < 64; ++i) {
            points[count] = (FT_Vector){
                lroundf((eye[side][i].x * 7.2f + dx - MASK_X) * 64),
                lroundf((MASK_HEIGHT - (eye[side][i].y * 7.2f + dy - MASK_Y)) * 64)};
            tags[count++] = FT_CURVE_TAG_ON;
        }
        contours[contour_count++] = (short)(count - 1);
        if(hole > .001f) {
            for(unsigned i = 0; i < 64; ++i) {
                float angle = (float)i * 6.28318530718f / 64;
                float x = (side ? 68 : 32) + cosf(angle) * 4.6f * hole;
                float y = 50 + sinf(angle) * 6.8f * hole;
                points[count] = (FT_Vector){lroundf((x * 7.2f + dx - MASK_X) * 64),
                    lroundf((MASK_HEIGHT - (y * 7.2f + dy - MASK_Y)) * 64)};
                tags[count++] = FT_CURVE_TAG_ON;
            }
            contours[contour_count++] = (short)(count - 1);
        }
    }
    FT_Outline outline = {.n_contours = (short)contour_count, .n_points = (short)count,
        .points = points, .tags = tags, .contours = contours, .flags = FT_OUTLINE_EVEN_ODD_FILL};
    FT_Bitmap bitmap = {.rows = MASK_HEIGHT, .width = MASK_WIDTH, .pitch = MASK_WIDTH,
        .buffer = saver->pixels, .num_grays = 256, .pixel_mode = FT_PIXEL_MODE_GRAY};
    memset(saver->pixels, 0, MASK_WIDTH * MASK_HEIGHT);
    (void)FT_Outline_Get_Bitmap(saver->rasterizer, &outline, &bitmap);
    lv_obj_invalidate(saver->eyes);
}

static void update_clock(face_t *saver)
{
    char time[16], text[64];
    snprintf(time, sizeof(time), "%s", saver->snapshot.time);
    snprintf(text, sizeof(text), "%s · 轻触唤醒", time);
    if(!strcmp(text, saver->time_text)) return;
    snprintf(saver->time_text, sizeof(saver->time_text), "%s", text);
    lv_label_set_text(saver->clock, text);
}

static void animate(face_t *saver, uint32_t now)
{
    const saver_frame_t *frame = current_frame(saver);
    uint32_t elapsed = now - saver->frame_at;
    if(elapsed >= frame->morph + frame->hold) {
        memcpy(saver->from, frame->eye, sizeof(saver->from));
        saver->from_hole = frame->hole;
        memcpy(saver->opacity_from, saver->opacity, sizeof(saver->opacity));
        if(++saver->frame >= saver_faces[saver->face].count) {
            saver->frame = 0;
            saver->face = saver->face ? 0 : (unsigned)lv_rand(1, 5);
        }
        saver->frame_at = now;
        elapsed = 0;
        frame = current_frame(saver);
    }
    uint32_t total = now - saver->started_at;
    float t = (total % 6000) / 6000.0f;
    float dx, dy;
    if(t < .3f) {
        float k = css_ease(t / .3f, false);
        dx = .6f * k; dy = -1.2f * k;
    } else if(t < .65f) {
        float k = css_ease((t - .3f) / .35f, false);
        dx = mix(.6f, -.6f, k); dy = mix(-1.2f, .8f, k);
    } else {
        float k = css_ease((t - .65f) / .35f, false);
        dx = mix(-.6f, 0, k); dy = mix(.8f, 0, k);
    }
    render_eyes(saver, elapsed, dx * 7.2f, dy * 7.2f);
    float k = css_ease(fminf(1, elapsed / 350.0f), true);
    for(unsigned i = 0; i < 4; ++i) {
        saver->opacity[i] = mix(saver->opacity_from[i], frame->fx == (saver_fx_t)(i + 1) ? 1 : 0, k);
        lv_obj_set_style_opa(saver->effects[i], lroundf(saver->opacity[i] * 255), 0);
        float y = dy * 7.2f;
        if(i == 3) {
            float phase = (total % 1600) / 800.0f;
            y -= 10.8f * css_ease(phase < 1 ? phase : 2 - phase, false);
        }
        lv_obj_set_pos(saver->effects[i], effects[i].x + lroundf(dx * 7.2f), effects[i].y + lroundf(y));
        if(i == 2) {
            float phase = (total % 600) / 300.0f;
            lv_image_set_scale(saver->effects[i], lroundf(256 * (1 + .25f * css_ease(phase < 1 ? phase : 2 - phase, false))));
        }
    }
    update_clock(saver);
}

static void *create(binbot_ambient_display_t *display, lv_obj_t *root, const ambient_theme_t *theme)
{
    (void)theme;
    face_t *saver = calloc(1, sizeof(*saver));
    if(!saver) return NULL;
    saver->display = display;
    saver->root = root;
    saver->pixels = calloc(1, MASK_WIDTH * MASK_HEIGHT);
    if(!saver->pixels || FT_Init_FreeType(&saver->rasterizer)) {
        free(saver->pixels);
        free(saver);
        return NULL;
    }
    saver->mask = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_A8, .w = MASK_WIDTH, .h = MASK_HEIGHT, .stride = MASK_WIDTH},
        .data_size = MASK_WIDTH * MASK_HEIGHT, .data = saver->pixels};
    saver->started_at = saver->frame_at = lv_tick_get();
    memcpy(saver->from, saver_faces[0].frames[0].eye, sizeof(saver->from));
    saver->eyes = lv_image_create(root);
    lv_obj_remove_flag(saver->eyes, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_image_set_src(saver->eyes, &saver->mask);
    lv_obj_set_pos(saver->eyes, MASK_X, MASK_Y);
    lv_obj_set_style_image_recolor(saver->eyes, lv_color_hex(0xffffff), 0);
    for(unsigned i = 0; i < 4; ++i) {
        char relative[160], path[640];
        snprintf(relative, sizeof(relative), "standby/face/%s.png", effects[i].name);
        ambient_asset_path(display, relative, path, sizeof(path));
        lv_image_header_t header;
        if(lv_image_decoder_get_info(path, &header) != LV_RESULT_OK) {
            snprintf(relative, sizeof(relative), "reference/screensaver/%s.png", effects[i].name);
            ambient_asset_path(display, relative, path, sizeof(path));
        }
        saver->effects[i] = lv_image_create(root);
        lv_image_set_src(saver->effects[i], path);
        lv_obj_remove_flag(saver->effects[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }
    saver->clock = ambient_text(display, root, "", 17, 400, 0x595959, 0, 661);
    lv_obj_set_width(saver->clock, 720);
    lv_obj_set_style_text_align(saver->clock, LV_TEXT_ALIGN_CENTER, 0);
    return saver;
}

static void update(void *context, const binbot_ambient_snapshot_t *snapshot)
{
    face_t *saver = context;
    saver->snapshot = *snapshot;
    update_clock(saver);
}

static void tick(void *context, uint32_t now) { animate(context, now); }

static void destroy(void *context)
{
    face_t *saver = context;
    lv_image_cache_drop(&saver->mask);
    if(saver->rasterizer) FT_Done_FreeType(saver->rasterizer);
    free(saver->pixels);
    free(saver);
}

const ambient_renderer_t ambient_face_renderer = {
    .id = "face", .create = create, .update = update, .tick = tick, .destroy = destroy,
};

