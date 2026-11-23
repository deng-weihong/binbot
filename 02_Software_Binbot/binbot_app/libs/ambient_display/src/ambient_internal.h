#ifndef BINBOT_AMBIENT_INTERNAL_H
#define BINBOT_AMBIENT_INTERNAL_H
#include "binbot/ambient_display.h"

typedef struct {
    binbot_ambient_theme_t info;
    char renderer[24], font[80], background_image[128];
    unsigned refresh_ms;
} ambient_theme_t;

typedef struct {
    const char *id;
    void *(*create)(binbot_ambient_display_t *display, lv_obj_t *root, const ambient_theme_t *theme);
    void (*update)(void *context, const binbot_ambient_snapshot_t *snapshot);
    void (*tick)(void *context, uint32_t now);
    void (*destroy)(void *context);
} ambient_renderer_t;

typedef struct ambient_frame ambient_frame_t;
struct binbot_ambient_display {
    binbot_ambient_config_t config;
    char asset_root[384];
    ambient_theme_t themes[BINBOT_AMBIENT_MAX_THEMES];
    size_t theme_count;
    ambient_frame_t *active, *retired;
    lv_timer_t *timer;
    binbot_ambient_snapshot_t snapshot;
    lv_obj_t *controls;
    uint32_t controls_at;
    lv_point_t pressed_at;
};

extern const ambient_renderer_t ambient_face_renderer;
extern const ambient_renderer_t ambient_scene_renderer;
extern const ambient_renderer_t ambient_reference_renderer;
extern const ambient_renderer_t ambient_pixel_renderer;
const ambient_renderer_t *ambient_renderer_find(const char *id);
bool ambient_registry_load(binbot_ambient_display_t *display);
bool ambient_safe_id(const char *id);
bool ambient_safe_path(const char *path);
lv_obj_t *ambient_box(lv_obj_t *parent, int x, int y, int width, int height,
                       uint32_t color, int radius);
lv_obj_t *ambient_text(binbot_ambient_display_t *display, lv_obj_t *parent,
                        const char *text, unsigned size, unsigned weight,
                        uint32_t color, int x, int y);
void ambient_text_set(lv_obj_t *label, const char *text);
void ambient_asset_path(const binbot_ambient_display_t *display, const char *relative,
                         char *path, size_t capacity);
#endif

