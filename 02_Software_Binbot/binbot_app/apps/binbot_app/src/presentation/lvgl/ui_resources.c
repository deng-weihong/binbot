#include "ui_internal.h"
#include "libs/freetype/lv_freetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fonts are loaded on first use. Widgets retain the fonts they actually use,
 * including hidden widgets and non-default parts; deletion releases them.
 * A deferred sweep also reclaims fonts used only for measuring text. */
enum { FONT_CAPACITY = 96 };
typedef struct {
    char path[96], fallback_path[96];
    uint16_t size;
    size_t users;
    lv_font_t *font, *fallback;
} resource_font_t;
typedef struct font_binding {
    resource_font_t *font;
    uint32_t selector;
    struct font_binding *next;
} font_binding_t;
typedef struct { font_binding_t *bindings; } font_owner_t;
static resource_font_t fonts[FONT_CAPACITY];
static bool sweep_pending;

static void release(resource_font_t *font)
{
    if(font->font) lv_freetype_font_delete(font->font);
    if(font->fallback) lv_freetype_font_delete(font->fallback);
    *font = (resource_font_t){0};
}

static void sweep(void *unused)
{
    (void)unused;
    sweep_pending = false;
    for(size_t i = 0; i < FONT_CAPACITY; ++i)
        if(fonts[i].font && !fonts[i].users) release(&fonts[i]);
}

static void schedule_sweep(void)
{
    if(!sweep_pending && lv_async_call(sweep, NULL) == LV_RESULT_OK) sweep_pending = true;
}

void ui_resources_collect(void)
{
    lv_async_call_cancel(sweep, NULL);
    sweep(NULL);
}

size_t ui_resources_font_count(void)
{
    size_t count = 0;
    for(size_t i = 0; i < FONT_CAPACITY; ++i) if(fonts[i].font) ++count;
    return count;
}

static lv_font_t *load(const char *relative, uint16_t size)
{
    if(!relative || !*relative) return NULL;
    const char *root = getenv("BINBOT_ASSET_ROOT");
    if(!root || !*root) root = "/oem/assets";
    if(root[0] == 'A' && root[1] == ':') root += 2;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                    size, LV_FREETYPE_FONT_STYLE_NORMAL);
}

const lv_font_t *ui_resources_font(const char *path, const char *fallback_path, uint16_t size)
{
    if(!path || !size || strlen(path) >= sizeof(fonts[0].path) ||
       (fallback_path && strlen(fallback_path) >= sizeof(fonts[0].fallback_path))) return LV_FONT_DEFAULT;
    const char *fallback = fallback_path ? fallback_path : "";
    resource_font_t *slot = NULL;
    for(size_t i = 0; i < FONT_CAPACITY; ++i) {
        if(fonts[i].font && fonts[i].size == size && !strcmp(fonts[i].path, path) &&
           !strcmp(fonts[i].fallback_path, fallback)) return fonts[i].font;
        if(!fonts[i].font && !slot) slot = &fonts[i];
    }
    if(!slot) { fprintf(stderr, "ui: live font limit reached\n"); return LV_FONT_DEFAULT; }
    lv_font_t *font = load(path, size), *other = load(fallback, size);
    if(!font) { font = other; other = NULL; }
    if(!font) return &lv_font_source_han_sans_sc_14_cjk;
    if(other) other->fallback = &lv_font_montserrat_20;
    font->fallback = other ? other : &lv_font_montserrat_20;
    *slot = (resource_font_t){.size = size, .font = font, .fallback = other};
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    snprintf(slot->fallback_path, sizeof(slot->fallback_path), "%s", fallback);
    schedule_sweep();
    return font;
}

static resource_font_t *managed_font(const lv_font_t *font)
{
    if(!font) return NULL;
    for(size_t i = 0; i < FONT_CAPACITY; ++i) if(fonts[i].font == font) return &fonts[i];
    return NULL;
}

static void owner_deleted(lv_event_t *event)
{
    font_owner_t *owner = lv_event_get_user_data(event);
    font_binding_t *binding = owner->bindings;
    while(binding) {
        font_binding_t *next = binding->next;
        if(binding->font && binding->font->users) --binding->font->users;
        free(binding);
        binding = next;
    }
    free(owner);
    schedule_sweep();
}

static font_owner_t *font_owner(lv_obj_t *object, bool create)
{
    for(uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
        lv_event_dsc_t *dsc = lv_obj_get_event_dsc(object, i);
        if(lv_event_dsc_get_cb(dsc) == owner_deleted) return lv_event_dsc_get_user_data(dsc);
    }
    if(!create) return NULL;
    font_owner_t *owner = calloc(1, sizeof(*owner));
    if(!owner) return NULL;
    if(!lv_obj_add_event_cb(object, owner_deleted, LV_EVENT_DELETE, owner)) {
        free(owner); return NULL;
    }
    return owner;
}

static bool bind_font(lv_obj_t *object, resource_font_t *font, uint32_t selector)
{
    font_owner_t *owner = font_owner(object, font != NULL);
    if(!owner) return font == NULL;
    font_binding_t *binding = owner->bindings;
    while(binding && binding->selector != selector) binding = binding->next;
    if(!binding) {
        if(!font) return true;
        binding = calloc(1, sizeof(*binding));
        if(!binding) return false;
        binding->selector = selector;
        binding->next = owner->bindings;
        owner->bindings = binding;
    }
    if(binding->font == font) return true;
    if(binding->font && binding->font->users) --binding->font->users;
    binding->font = font;
    if(font) ++font->users;
    schedule_sweep();
    return true;
}

void ui_resources_set_font(lv_obj_t *object, const lv_font_t *font, lv_style_selector_t selector)
{
    if(!object) return;
    if(!font) font = LV_FONT_DEFAULT;
    resource_font_t *managed = managed_font(font);
    if(!bind_font(object, managed, selector)) {
        /* Keep the old style and its ownership if a binding cannot be allocated. */
        fprintf(stderr, "ui: cannot retain font\n"); return;
    }
    lv_obj_set_style_text_font(object, font, selector);
}

bool ui_resources_retain_font(lv_obj_t *owner, const lv_font_t *font)
{
    resource_font_t *managed = managed_font(font);
    if(!managed) return true;
    /* An explicit loan may coexist with styles on any part and state. */
    uint32_t key = 0x80000000U | (uint32_t)(managed - fonts);
    return bind_font(owner, managed, key);
}

void ui_resources_shutdown(void)
{
    lv_async_call_cancel(sweep, NULL);
    sweep_pending = false;
    for(size_t i = 0; i < FONT_CAPACITY; ++i) {
        if(fonts[i].users) fprintf(stderr, "ui: shutdown with a live font owner\n");
        release(&fonts[i]);
    }
}
