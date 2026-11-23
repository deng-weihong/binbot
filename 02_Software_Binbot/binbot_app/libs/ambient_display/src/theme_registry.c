#include "ambient_internal.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ambient_safe_id(const char *id)
{
    if(!id || !*id || strlen(id) >= BINBOT_AMBIENT_ID_SIZE) return false;
    for(const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

bool ambient_safe_path(const char *path)
{
    return path && *path && path[0] != '/' && !strstr(path, "..") &&
           !strchr(path, ':') && !strchr(path, '\\');
}

const ambient_renderer_t *ambient_renderer_find(const char *id)
{
    if(id && !strcmp(id, ambient_face_renderer.id)) return &ambient_face_renderer;
    if(id && !strcmp(id, ambient_scene_renderer.id)) return &ambient_scene_renderer;
    if(id && !strcmp(id, ambient_reference_renderer.id)) return &ambient_reference_renderer;
    if(id && !strcmp(id, ambient_pixel_renderer.id)) return &ambient_pixel_renderer;
    return NULL;
}

static cJSON *read_json(const char *path)
{
    FILE *file = fopen(path, "rb");
    if(!file) return NULL;
    char *text = malloc(65537);
    if(!text) { fclose(file); return NULL; }
    size_t size = fread(text, 1, 65537, file);
    bool valid = size <= 65536 && !ferror(file);
    fclose(file);
    cJSON *json = NULL;
    if(valid) { text[size] = 0; json = cJSON_ParseWithLength(text, size + 1); }
    free(text);
    return json;
}

static const char *string(cJSON *object, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool color(cJSON *object, const char *key, uint32_t *output)
{
    const char *value = string(object, key);
    if(!value || strlen(value) != 7 || *value++ != '#') return false;
    for(unsigned i = 0; i < 6; ++i) if(!isxdigit((unsigned char)value[i])) return false;
    *output = (uint32_t)strtoul(value, NULL, 16);
    return true;
}

bool ambient_registry_load(binbot_ambient_display_t *display)
{
    memset(display->themes, 0, sizeof(display->themes));
    display->theme_count = 1;
    display->themes[0] = (ambient_theme_t){
        .info = {.id = "face", .name = "小伴表情", .description = "眨眼、微笑与陪伴",
                 .background = 0x000000, .foreground = 0xffffff, .accent = 0xe8643c},
        .renderer = "face", .refresh_ms = 33,
    };
    const char *root = display->asset_root;
    if(root[0] == 'A' && root[1] == ':') root += 2;
    char path[640];
    snprintf(path, sizeof(path), "%s/standby/index.json", root);
    cJSON *index = read_json(path);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(index, "schema");
    cJSON *list = cJSON_GetObjectItemCaseSensitive(index, "themes");
    bool valid_index = cJSON_IsNumber(version) && version->valuedouble == 1 && cJSON_IsArray(list);
    if(!valid_index) { cJSON_Delete(index); return false; }
    cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if(!cJSON_IsString(item) || !ambient_safe_id(item->valuestring)) continue;
        const char *id = item->valuestring;
        snprintf(path, sizeof(path), "%s/standby/%s/manifest.json", root, id);
        cJSON *manifest = read_json(path);
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(manifest, "schema");
        const char *renderer = string(manifest, "renderer");
        const char *name = string(manifest, "name");
        const char *manifest_id = string(manifest, "id");
        const char *description = string(manifest, "description");
        const char *english = string(manifest, "english");
        const char *font = string(manifest, "font");
        const char *background = string(manifest, "background_image");
        bool valid = cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
            manifest_id && !strcmp(id, manifest_id) && name && strlen(name) < 64 &&
            renderer && ambient_renderer_find(renderer) &&
            (!background || (strlen(background) < 128 && ambient_safe_path(background))) &&
            (!font || (strlen(font) < 80 && ambient_safe_path(font)));
        if(valid) {
            size_t slot = display->theme_count;
            for(size_t i = 0; i < display->theme_count; ++i)
                if(!strcmp(display->themes[i].info.id, id)) { slot = i; break; }
            bool is_face_id = !strcmp(id, "face");
            bool is_face_renderer = !strcmp(renderer, "face");
            if(slot < BINBOT_AMBIENT_MAX_THEMES && is_face_id == is_face_renderer) {
                ambient_theme_t theme = {
                    .info = {.background = 0x101014, .foreground = 0xffffff, .accent = 0xe8643c},
                    .refresh_ms = 1000,
                };
                snprintf(theme.info.id, sizeof(theme.info.id), "%s", id);
                snprintf(theme.info.name, sizeof(theme.info.name), "%s", name);
                snprintf(theme.info.english, sizeof(theme.info.english), "%s", english ? english : "");
                snprintf(theme.info.description, sizeof(theme.info.description), "%s", description ? description : "");
                snprintf(theme.renderer, sizeof(theme.renderer), "%s", renderer);
                if(font) snprintf(theme.font, sizeof(theme.font), "%s", font);
                if(background) snprintf(theme.background_image, sizeof(theme.background_image), "%s", background);
                (void)color(manifest, "background", &theme.info.background);
                (void)color(manifest, "foreground", &theme.info.foreground);
                (void)color(manifest, "accent", &theme.info.accent);
                cJSON *refresh = cJSON_GetObjectItemCaseSensitive(manifest, "refresh_ms");
                if(cJSON_IsNumber(refresh) && refresh->valuedouble >= 33 && refresh->valuedouble <= 1000)
                    theme.refresh_ms = (unsigned)refresh->valuedouble;
                if(!strcmp(renderer, "face")) theme.refresh_ms = 33;
                display->themes[slot] = theme;
                if(slot == display->theme_count) ++display->theme_count;
            }
        }
        cJSON_Delete(manifest);
    }
    cJSON_Delete(index);
    return true;
}

