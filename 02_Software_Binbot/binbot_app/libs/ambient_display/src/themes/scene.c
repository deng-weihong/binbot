#include "../ambient_internal.h"
#include "libs/freetype/lv_freetype.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A deliberately small scene vocabulary. Layout lives in the resource package;
 * networking, state and navigation never enter the renderer. */
enum { MAX_LAYERS = 64, MAX_FONTS = 20 };
typedef enum {
    B_NONE, B_TIME, B_HOUR, B_MINUTE, B_SECOND, B_DATE, B_CITY,
    B_WEATHER, B_TEMPERATURE, B_WEATHER_LINE, B_WEATHER_RANGE, B_WEATHER_ICON,
    B_TRACK, B_ARTIST, B_MEDIA_STATE, B_TODO, B_TODO_PROGRESS,
    B_NEXT_TODO, B_FOCUS, B_FOCUS_STATE, B_COUNT
} binding_t;
static const char *const binding_names[B_COUNT] = {
    "", "time", "hour", "minute", "second", "date", "city", "weather",
    "temperature", "weather_line", "weather_range", "weather_icon", "track", "artist",
    "media_state", "todo", "todo_progress", "next_todo", "focus", "focus_state"
};
typedef struct {
    lv_obj_t *object;
    binding_t binding;
    bool image, progress, blink;
    uint8_t opacity;
    char source[640];
} layer_t;
typedef struct {
    unsigned size, weight;
    lv_font_t *font, *fallback;
} font_t;
typedef struct {
    binbot_ambient_display_t *display;
    const ambient_theme_t *theme;
    lv_obj_t *root;
    layer_t layers[MAX_LAYERS];
    size_t count, font_count;
    font_t fonts[MAX_FONTS];
    bool blink_on;
} scene_t;

static const char *string(cJSON *object, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool number(cJSON *o, const char *key, int fallback, int low, int high, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if(!v) { *out = fallback; return true; }
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
       v->valuedouble > high || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (int)v->valuedouble;
    return true;
}

static bool color(cJSON *o, const char *key, uint32_t fallback, uint32_t *out)
{
    const char *s = string(o, key);
    *out = fallback;
    if(!cJSON_GetObjectItemCaseSensitive(o, key)) return true;
    if(!s || strlen(s) != 7 || s[0] != '#') return false;
    for(unsigned i = 1; i < 7; ++i) if(!isxdigit((unsigned char)s[i])) return false;
    char *end;
    unsigned long value = strtoul(s + 1, &end, 16);
    if(*end || end != s + 7) return false;
    *out = (uint32_t)value;
    return true;
}

static const lv_font_t *scene_font(scene_t *scene, unsigned size, unsigned weight)
{
    for(size_t i = 0; i < scene->font_count; ++i)
        if(scene->fonts[i].size == size && scene->fonts[i].weight == weight) return scene->fonts[i].font;
    if(scene->font_count == MAX_FONTS) return NULL;
    const char *root = scene->display->asset_root;
    if(root[0] == 'A' && root[1] == ':') root += 2;
    char path[640];
    if(scene->theme->font[0]) snprintf(path, sizeof(path), "%s/%s", root, scene->theme->font);
    else snprintf(path, sizeof(path), "%s/fonts/InterTight-%s.ttf", root,
                  weight >= 600 ? "SemiBold" : weight >= 500 ? "Medium" : "Regular");
    lv_font_t *font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!font && scene->theme->font[0]) return NULL;
    snprintf(path, sizeof(path), "%s/fonts/NotoSansSC-%s.ttf", root,
             weight >= 600 ? "SemiBold" : weight >= 500 ? "Medium" : "Regular");
    lv_font_t *fallback = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                   size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!font) { font = fallback; fallback = NULL; }
    if(!font) return scene->display->config.font ?
        scene->display->config.font(scene->display->config.user_data, scene->root, (uint16_t)size, weight) : LV_FONT_DEFAULT;
    if(fallback) fallback->fallback = LV_FONT_DEFAULT;
    font->fallback = fallback ? fallback : LV_FONT_DEFAULT;
    scene->fonts[scene->font_count++] = (font_t){size, weight, font, fallback};
    return font;
}

static bool image_source(layer_t *layer, const char *path)
{
    if(!strcmp(layer->source, path)) return true;
    lv_image_header_t header;
    if(lv_image_decoder_get_info(path, &header) != LV_RESULT_OK ||
       !header.w || !header.h || header.w > 720 || header.h > 720) return false;
    snprintf(layer->source, sizeof(layer->source), "%s", path);
    lv_image_set_src(layer->object, layer->source);
    return true;
}

static bool add_layer(scene_t *s, cJSON *json)
{
    const char *type = string(json, "type"), *binding = string(json, "bind");
    if(!type || s->count == MAX_LAYERS) return false;
    binding_t b = B_NONE;
    if(binding) {
        for(b = B_TIME; b < B_COUNT; ++b) if(!strcmp(binding_names[b], binding)) break;
        if(b == B_COUNT) return false;
    }
    int x, y, w, h, size, weight, radius, opacity;
    uint32_t fg;
    if(!number(json, "x", 0, 0, 719, &x) || !number(json, "y", 0, 0, 719, &y) ||
       !number(json, "w", 720 - x, 1, 720 - x, &w) ||
       !number(json, "h", 720 - y, 1, 720 - y, &h) ||
       !number(json, "size", 24, 10, 240, &size) ||
       !number(json, "weight", 400, 400, 700, &weight) ||
       !number(json, "radius", 0, 0, 360, &radius) ||
       !number(json, "opacity", 255, 0, 255, &opacity) ||
       !color(json, "color", s->theme->info.foreground, &fg)) return false;
    layer_t *layer = &s->layers[s->count++];
    layer->binding = b;
    layer->opacity = (uint8_t)opacity;
    cJSON *blink = cJSON_GetObjectItemCaseSensitive(json, "blink");
    if(blink && !cJSON_IsBool(blink)) return false;
    layer->blink = cJSON_IsTrue(blink);
    if(!strcmp(type, "box")) {
        if(b != B_NONE) return false;
        layer->object = ambient_box(s->root, x, y, w, h, fg, radius);
    }
    else if(!strcmp(type, "text")) {
        if(b == B_WEATHER_ICON || b == B_TODO_PROGRESS) return false;
        const char *text = string(json, "text"), *align = string(json, "align");
        if((!text && b == B_NONE) || (text && strlen(text) > 512)) return false;
        const lv_font_t *font = scene_font(s, (unsigned)size, (unsigned)weight);
        if(!font) return false;
        layer->object = lv_label_create(s->root);
        lv_label_set_text(layer->object, text ? text : "");
        lv_label_set_long_mode(layer->object, LV_LABEL_LONG_DOT);
        lv_obj_set_size(layer->object, w, h);
        lv_obj_set_style_text_font(layer->object, font, 0);
        lv_obj_set_style_text_color(layer->object, lv_color_hex(fg), 0);
        if(align && strcmp(align, "left") && strcmp(align, "center") && strcmp(align, "right")) return false;
        lv_obj_set_style_text_align(layer->object,
            align && !strcmp(align, "center") ? LV_TEXT_ALIGN_CENTER :
            align && !strcmp(align, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
    else if(!strcmp(type, "image")) {
        const char *src = string(json, "src");
        if(b != B_NONE && b != B_WEATHER_ICON) return false;
        layer->image = true;
        layer->object = lv_image_create(s->root);
        lv_obj_set_size(layer->object, w, h);
        lv_image_set_inner_align(layer->object, LV_IMAGE_ALIGN_CONTAIN);
        if(b == B_NONE) {
            if(!src || !ambient_safe_path(src)) return false;
            char path[640];
            ambient_asset_path(s->display, src, path, sizeof(path));
            if(!image_source(layer, path)) return false;
        }
    }
    else if(!strcmp(type, "progress")) {
        if(b != B_TODO_PROGRESS) return false;
        uint32_t track;
        if(!color(json, "track_color", 0x252525, &track)) return false;
        layer->progress = true;
        layer->object = lv_bar_create(s->root);
        lv_obj_remove_style_all(layer->object);
        lv_obj_set_size(layer->object, w, h);
        lv_bar_set_range(layer->object, 0, 100);
        lv_obj_set_style_bg_color(layer->object, lv_color_hex(track), 0);
        lv_obj_set_style_bg_opa(layer->object, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(layer->object, radius, 0);
        lv_obj_set_style_bg_color(layer->object, lv_color_hex(fg), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(layer->object, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(layer->object, radius, LV_PART_INDICATOR);
    }
    else return false;
    if(!layer->object) return false;
    lv_obj_set_pos(layer->object, x, y);
    lv_obj_set_style_opa(layer->object, opacity, 0);
    lv_obj_remove_flag(layer->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return true;
}

static void destroy(void *context)
{
    scene_t *s = context;
    for(size_t i = 0; i < s->font_count; ++i) {
        lv_freetype_font_delete(s->fonts[i].font);
        if(s->fonts[i].fallback) lv_freetype_font_delete(s->fonts[i].fallback);
    }
    for(size_t i = 0; i < s->count; ++i)
        if(s->layers[i].source[0]) lv_image_cache_drop(s->layers[i].source);
    free(s);
}

static void *create(binbot_ambient_display_t *display, lv_obj_t *root, const ambient_theme_t *theme)
{
    const char *asset_root = display->asset_root;
    if(asset_root[0] == 'A' && asset_root[1] == ':') asset_root += 2;
    char path[640];
    snprintf(path, sizeof(path), "%s/standby/%s/scene.json", asset_root, theme->info.id);
    FILE *file = fopen(path, "rb");
    if(!file) return NULL;
    char *text = malloc(65537);
    if(!text) { fclose(file); return NULL; }
    size_t n = fread(text, 1, 65537, file);
    bool readable = n <= 65536 && !ferror(file);
    fclose(file);
    cJSON *json = NULL;
    if(readable) { text[n] = 0; json = cJSON_ParseWithLength(text, n + 1); }
    free(text);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    cJSON *layers = cJSON_GetObjectItemCaseSensitive(json, "layers");
    if(!cJSON_IsNumber(schema) || schema->valuedouble != 1 || !cJSON_IsArray(layers) ||
       cJSON_GetArraySize(layers) < 1 || cJSON_GetArraySize(layers) > MAX_LAYERS - 1) {
        cJSON_Delete(json); return NULL;
    }
    scene_t *s = calloc(1, sizeof(*s));
    if(!s) { cJSON_Delete(json); return NULL; }
    s->display = display; s->theme = theme; s->root = root;
    bool valid = true;
    if(theme->background_image[0]) {
        layer_t *layer = &s->layers[s->count++];
        layer->object = lv_image_create(root);
        layer->image = true;
        ambient_asset_path(display, theme->background_image, path, sizeof(path));
        valid = image_source(layer, path);
        lv_obj_set_size(layer->object, 720, 720);
        lv_image_set_inner_align(layer->object, LV_IMAGE_ALIGN_CONTAIN);
    }
    cJSON *item;
    cJSON_ArrayForEach(item, layers) if(valid) valid = add_layer(s, item);
    cJSON_Delete(json);
    if(!valid) {
        /* Fonts must outlive labels, including a partially constructed scene. */
        lv_obj_clean(root);
        destroy(s);
        return NULL;
    }
    return s;
}

static void value(binding_t b, const binbot_ambient_snapshot_t *d, char *out, size_t capacity)
{
    const char *text = "";
    switch(b) {
        case B_TIME: text = d->time; break;
        case B_HOUR: snprintf(out, capacity, "%02d", d->hour); return;
        case B_MINUTE: snprintf(out, capacity, "%02d", d->minute); return;
        case B_SECOND: snprintf(out, capacity, "%02d", d->second); return;
        case B_DATE: text = d->date; break;
        case B_CITY: text = d->city; break;
        case B_WEATHER: text = d->weather_valid ? d->weather : "天气暂未更新"; break;
        case B_TEMPERATURE:
            if(d->weather_valid) snprintf(out, capacity, "%d°", d->temperature);
            else snprintf(out, capacity, "--°");
            return;
        case B_WEATHER_LINE:
            if(d->weather_valid) snprintf(out, capacity, "%s  %d°", d->weather, d->temperature);
            else snprintf(out, capacity, "天气暂未更新");
            return;
        case B_WEATHER_RANGE:
            if(d->weather_valid) snprintf(out, capacity, "最低 %d°  /  最高 %d°", d->temperature_low, d->temperature_high);
            else snprintf(out, capacity, "最低 --°  /  最高 --°");
            return;
        case B_TRACK: text = d->track[0] ? d->track : "此刻，留一点安静"; break;
        case B_ARTIST: text = d->artist; break;
        case B_MEDIA_STATE: text = d->playing ? "正在播放" : "音乐已暂停"; break;
        case B_TODO: snprintf(out, capacity, "%u / %u", d->todo_done, d->todo_total); return;
        case B_NEXT_TODO: text = d->next_todo[0] ? d->next_todo : "今天的待办已完成"; break;
        case B_FOCUS:
            snprintf(out, capacity, "%02u:%02u", d->focus_seconds / 60, d->focus_seconds % 60); return;
        case B_FOCUS_STATE: text = d->focus_running ? "专注进行中" : "准备好，慢慢开始"; break;
        default: break;
    }
    snprintf(out, capacity, "%s", text);
}

static void update(void *context, const binbot_ambient_snapshot_t *data)
{
    scene_t *s = context;
    for(size_t i = 0; i < s->count; ++i) {
        layer_t *layer = &s->layers[i];
        if(layer->binding == B_NONE) continue;
        if(layer->image) {
            bool valid = data->weather_valid && ambient_safe_path(data->weather_icon);
            if(valid) {
                char path[640];
                ambient_asset_path(s->display, data->weather_icon, path, sizeof(path));
                valid = image_source(layer, path);
            }
            lv_obj_set_flag(layer->object, LV_OBJ_FLAG_HIDDEN, !valid);
        }
        else if(layer->progress) {
            uint32_t done = data->todo_done > data->todo_total ? data->todo_total : data->todo_done;
            int percent = data->todo_total ? (int)(100ULL * done / data->todo_total) : 0;
            if(lv_bar_get_value(layer->object) != percent) lv_bar_set_value(layer->object, percent, LV_ANIM_OFF);
        }
        else {
            char text[256];
            value(layer->binding, data, text, sizeof(text));
            ambient_text_set(layer->object, text);
        }
    }
}

static void tick(void *context, uint32_t now)
{
    scene_t *s = context;
    bool on = (now / 600) % 2 == 0;
    if(on == s->blink_on) return;
    s->blink_on = on;
    for(size_t i = 0; i < s->count; ++i) if(s->layers[i].blink)
        lv_obj_set_style_opa(s->layers[i].object, on ? s->layers[i].opacity : 0, 0);
}

const ambient_renderer_t ambient_scene_renderer = {
    .id = "scene", .create = create, .update = update, .tick = tick, .destroy = destroy,
};
