#include "../ui_internal.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct saver_settings saver_settings_t;
typedef struct { saver_settings_t *screen; unsigned command; } saver_action_t;
enum { DELAY_FIRST = 32, SHUFFLE = 37, PREVIEW, THUMB = 152, ROW_PITCH = 213 };
static const uint16_t delays[] = {30, 60, 300, 600, 0};
struct saver_settings {
    ui_app_t *app;
    lv_obj_t *root, *list, *status, *shuffle, *shuffle_label;
    lv_obj_t *cards[32], *frames[32], *clips[32], *images[32], *names[32], *checks[32];
    lv_obj_t *delay_buttons[5], *delay_names[5];
    lv_draw_buf_t *thumbnails[32];
    saver_action_t actions[40];
    lv_timer_t *loader;
    size_t count;
    uint32_t scrolled_at;
    bool paused;
};

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = ui_box(parent, x, y, w, h, 0xffffff, 25);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0xf6f6f6), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(0), 0);
    lv_obj_set_style_shadow_width(o, 24, 0);
    lv_obj_set_style_shadow_opa(o, 12, 0);
    lv_obj_set_style_shadow_ofs_y(o, 8, 0);
    return o;
}

static void thumbnail_clear(saver_settings_t *s, size_t i)
{
    if(s->images[i]) { lv_obj_delete(s->images[i]); s->images[i] = NULL; }
    if(s->thumbnails[i]) {
        lv_image_cache_drop(s->thumbnails[i]);
        lv_draw_buf_destroy(s->thumbnails[i]); s->thumbnails[i] = NULL;
    }
}

static lv_draw_buf_t *thumbnail_load(const char *id)
{
    const char *assets = getenv("BINBOT_ASSET_ROOT");
    if(!assets || !*assets) assets = "/oem/assets";
    if(assets[0] == 'A' && assets[1] == ':') assets += 2;
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/standby/previews/%s.rgb565", assets, id);
    if(length < 0 || (size_t)length >= sizeof(path)) return NULL;
    FILE *file = fopen(path, "rb");
    if(!file) return NULL;
    lv_draw_buf_t *buffer = lv_draw_buf_create(THUMB, THUMB, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    bool valid = buffer != NULL;
    for(unsigned y = 0; valid && y < THUMB; ++y)
        valid = fread(buffer->data + y * buffer->header.stride, 1, THUMB * 2, file) == THUMB * 2;
    if(valid) valid = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if(!valid) { if(buffer) lv_draw_buf_destroy(buffer); return NULL; }
    return buffer;
}

static bool thumbnail_visible(saver_settings_t *s, size_t i)
{
    int top = 6 + (int)(i / 4) * ROW_PITCH - lv_obj_get_scroll_y(s->list);
    return top < lv_obj_get_height(s->list) && top + THUMB > 0;
}

static void thumbnail_make(saver_settings_t *s, size_t i)
{
    const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(ui_screensaver_display(s->app->screensaver), i);
    if(!theme) return;
    s->thumbnails[i] = thumbnail_load(theme->id);
    if(s->thumbnails[i]) {
        s->images[i] = lv_image_create(s->clips[i]);
        lv_image_set_src(s->images[i], s->thumbnails[i]);
        lv_obj_remove_flag(s->images[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

static void load_visible(lv_timer_t *timer)
{
    saver_settings_t *s = lv_timer_get_user_data(timer);
    if(s->paused) { lv_timer_pause(timer); return; }
    if(lv_tick_elaps(s->scrolled_at) < 140) return;
    for(size_t i = 0; i < s->count; ++i)
        if(!thumbnail_visible(s, i)) thumbnail_clear(s, i);
    for(size_t i = 0; i < s->count; ++i) {
        if(thumbnail_visible(s, i) && !s->thumbnails[i]) {
            thumbnail_make(s, i);
            /* A failed allocation must not leave a busy retry loop. */
            if(!s->thumbnails[i]) lv_timer_pause(timer);
            return;
        }
    }
    lv_timer_pause(timer);
}

static void scrolled(lv_event_t *event)
{
    saver_settings_t *s = lv_event_get_user_data(event);
    s->scrolled_at = lv_tick_get();
    for(size_t i = 0; i < s->count; ++i)
        if(!thumbnail_visible(s, i)) thumbnail_clear(s, i);
    if(!s->paused) lv_timer_resume(s->loader);
}

static void sync(saver_settings_t *s)
{
    const app_screensaver_settings_t *cfg = &app_state_model(s->app->state)->screensaver;
    binbot_ambient_display_t *display = ui_screensaver_display(s->app->screensaver);
    bool face = !strcmp(cfg->theme_id, "face");
    const binbot_ambient_theme_t *selected = binbot_ambient_theme_find(display, cfg->theme_id);
    if(!selected) selected = binbot_ambient_theme_at(display, 0);
    if(!selected) return;
    for(size_t i = 0; i < s->count; ++i) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(display, i);
        bool on = theme == selected && (face || !cfg->shuffle);
        lv_obj_set_style_outline_color(s->frames[i], lv_color_hex(on ? UI_REF_CLAY : 0), 0);
        lv_obj_set_style_outline_width(s->frames[i], on ? 3 : 1, 0);
        lv_obj_set_style_outline_pad(s->frames[i], on ? 3 : 0, 0);
        lv_obj_set_style_outline_opa(s->frames[i], on ? 255 : 13, 0);
        lv_obj_set_style_shadow_color(s->frames[i], lv_color_hex(on ? UI_REF_CLAY : 0), 0);
        lv_obj_set_style_shadow_opa(s->frames[i], on ? 120 : 64, 0);
        lv_obj_set_style_text_color(s->names[i], lv_color_hex(on ? UI_REF_CLAY : UI_REF_INK), 0);
        lv_obj_set_flag(s->checks[i], LV_OBJ_FLAG_HIDDEN, !on);
    }
    if(cfg->shuffle && !face) lv_label_set_text(s->status, "每次显示时从全部表盘中随机挑选");
    else lv_label_set_text_fmt(s->status, "正在使用  #171717 %s#", face ? "小伴表情" : selected->name);
    lv_obj_set_style_text_color(s->shuffle_label, lv_color_hex(face ? UI_REF_FAINT : 0x525252), 0);
    for(unsigned i = 0; i < 5; ++i) {
        bool on = cfg->delay_seconds == delays[i];
        lv_obj_set_style_bg_color(s->delay_buttons[i], lv_color_hex(on ? UI_REF_INK : 0xe6e6e6), 0);
        lv_obj_set_style_text_color(s->delay_names[i], lv_color_hex(on ? 0xffffff : UI_REF_INK), 0);
    }
    lv_obj_set_style_bg_color(s->shuffle, lv_color_hex(cfg->shuffle ? UI_REF_CLAY : 0xd6d6d6), 0);
    lv_obj_set_x(lv_obj_get_child(s->shuffle, 0), cfg->shuffle ? 29 : 4);
    if(!s->paused) lv_timer_resume(s->loader);
}

static void clicked(lv_event_t *event)
{
    saver_action_t *action = lv_event_get_user_data(event);
    saver_settings_t *s = action->screen;
    unsigned command = action->command;
    app_screensaver_settings_t cfg = app_state_model(s->app->state)->screensaver;
    if(command == PREVIEW) { ui_screensaver_show(s->app->screensaver); return; }
    if(command >= DELAY_FIRST && command < DELAY_FIRST + 5) cfg.delay_seconds = delays[command - DELAY_FIRST];
    else if(command == SHUFFLE) {
        cfg.shuffle = !cfg.shuffle;
        if(cfg.shuffle && !strcmp(cfg.theme_id, "face")) {
            const binbot_ambient_theme_t *theme = binbot_ambient_theme_find(ui_screensaver_display(s->app->screensaver), cfg.standby_id);
            if(!theme || !strcmp(theme->id, "face")) theme = binbot_ambient_theme_at(ui_screensaver_display(s->app->screensaver), 1);
            if(theme) snprintf(cfg.theme_id, sizeof(cfg.theme_id), "%s", theme->id);
        }
    }
    else if(command < s->count) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(ui_screensaver_display(s->app->screensaver), command);
        snprintf(cfg.theme_id, sizeof(cfg.theme_id), "%s", theme->id);
        if(command) snprintf(cfg.standby_id, sizeof(cfg.standby_id), "%s", theme->id);
        cfg.shuffle = false;
    }
    else return;
    app_state_set_screensaver(s->app->state, &cfg);
}

static void bind(saver_settings_t *s, lv_obj_t *object, unsigned command)
{
    s->actions[command] = (saver_action_t){s, command};
    lv_obj_add_event_cb(object, clicked, LV_EVENT_CLICKED, &s->actions[command]);
}

static void ring_extent(lv_event_t *event)
{
    int32_t *size = lv_event_get_param(event);
    if(*size < 6) *size = 6;
}

static void update(void *context, const app_model_t *model, uint32_t changes)
{
    (void)model;
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_SCREENSAVER)) sync(context);
}

static void paused(void *context, bool pause)
{
    saver_settings_t *s = context;
    s->paused = pause;
    if(pause) {
        lv_timer_pause(s->loader);
        for(size_t i = 0; i < s->count; ++i) thumbnail_clear(s, i);
    }
    else sync(s);
}

static void destroy(void *context)
{
    saver_settings_t *s = context;
    lv_timer_delete(s->loader);
    for(size_t i = 0; i < s->count; ++i) thumbnail_clear(s, i);
    free(s);
}

ui_screen_t ui_screensaver_settings_screen_create(ui_app_t *app)
{
    saver_settings_t *s = calloc(1, sizeof(*s));
    if(!s) return (ui_screen_t){0};
    s->app = app;
    s->root = ui_ref_shell_latest(app, "表盘", "WATCH FACE");
    binbot_ambient_display_t *display = ui_screensaver_display(app->screensaver);
    s->count = binbot_ambient_theme_count(display);
    s->loader = lv_timer_create(load_visible, 30, s);
    if(!s->loader) { lv_obj_delete(s->root); free(s); return (ui_screen_t){0}; }
    lv_timer_pause(s->loader);
    lv_obj_t *preview = ui_button(s->root, 565, 27, 126, 48, UI_REF_INK, 24);
    lv_obj_t *play = ui_ref_icon(preview, "play", 18, 0xffffff); lv_obj_set_pos(play, 16, 15);
    ui_ref_label(preview, "立即预览", 16, UI_FONT_SEMIBOLD, 0xffffff, 42, 12);
    bind(s, preview, PREVIEW);
    lv_obj_t *options = card(s->root, 22, 96, 676, 68);
    ui_ref_label(options, "闲置后显示", 16, UI_FONT_MEDIUM, UI_REF_INK, 18, 22);
    const char *delay_names[] = {"30 秒", "1 分钟", "5 分钟", "10 分钟", "从不"};
    const int widths[] = {62, 64, 67, 72, 56};
    int x = 110;
    for(unsigned i = 0; i < 5; ++i) {
        s->delay_buttons[i] = ui_button(options, x, 14, widths[i], 40, 0xe6e6e6, 20);
        s->delay_names[i] = ui_centered_label(s->delay_buttons[i], delay_names[i], ui_ref_font(14, UI_FONT_MEDIUM), UI_REF_INK);
        bind(s, s->delay_buttons[i], DELAY_FIRST + i);
        x += widths[i] + 6;
    }
    lv_obj_t *divider = ui_box(options, 512, 18, 1, 32, 0, 0);
    lv_obj_set_style_bg_opa(divider, 26, 0);
    s->shuffle_label = ui_ref_label(options, "随机轮换", 15, UI_FONT_REGULAR, 0x525252, 525, 23);
    s->shuffle = ui_button(options, 597, 16, 61, 36, 0xd6d6d6, 18);
    ui_box(s->shuffle, 4, 4, 29, 29, 0xffffff, 15); bind(s, s->shuffle, SHUFFLE);
    ui_ref_label(s->root, "选择表盘", 17, UI_FONT_MEDIUM, UI_REF_INK, 26, 179);
    char count[24]; snprintf(count, sizeof(count), "%u 款", (unsigned)s->count);
    ui_ref_label(s->root, count, 15, UI_FONT_REGULAR, UI_REF_FAINT, 100, 182);
    s->status = ui_ref_label(s->root, "", 14, UI_FONT_REGULAR, UI_REF_MUTED, 354, 182);
    lv_obj_set_width(s->status, 340);
    lv_obj_set_style_text_align(s->status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_recolor(s->status, true);
    s->list = ui_box(s->root, 18, 217, 684, 481, 0, 0);
    lv_obj_set_style_bg_opa(s->list, 0, 0);
    lv_obj_add_flag(s->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s->list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(s->list, 32, 0);
    static const int columns[] = {4, 179, 353, 528};
    size_t selected_index = 0;
    const app_screensaver_settings_t *cfg = &app_state_model(app->state)->screensaver;
    for(size_t i = 0; i < s->count; ++i) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(display, i);
        if(!strcmp(theme->id, cfg->theme_id)) selected_index = i;
        s->cards[i] = ui_button(s->list, columns[i % 4], 6 + (int)(i / 4) * ROW_PITCH, THUMB, 197, 0, 0);
        lv_obj_set_style_bg_opa(s->cards[i], 0, 0);
        lv_obj_add_flag(s->cards[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(s->cards[i], ring_extent, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(s->cards[i]);
        s->frames[i] = ui_box(s->cards[i], 0, 0, THUMB, THUMB, 0x141414, 22);
        lv_obj_set_style_shadow_width(s->frames[i], 24, 0);
        lv_obj_set_style_shadow_spread(s->frames[i], -16, 0);
        lv_obj_set_style_shadow_ofs_y(s->frames[i], 10, 0);
        s->clips[i] = ui_box(s->frames[i], 0, 0, THUMB, THUMB, 0x141414, 22);
        lv_obj_set_style_clip_corner(s->clips[i], true, 0);
        s->checks[i] = ui_box(s->frames[i], 114, 10, 28, 28, UI_REF_CLAY, 14);
        lv_obj_t *check = ui_ref_icon(s->checks[i], "check", 16, 0xffffff); lv_obj_center(check);
        s->names[i] = ui_ref_label(s->cards[i], theme->name, 15, UI_FONT_SEMIBOLD, UI_REF_INK, 4, 160);
        lv_obj_set_width(s->names[i], 144); lv_label_set_long_mode(s->names[i], LV_LABEL_LONG_DOT);
        lv_obj_t *en = ui_ref_label(s->cards[i], i ? theme->english : "Expressions", 12, UI_FONT_REGULAR, UI_REF_FAINT, 4, 180);
        lv_obj_set_width(en, 144); lv_label_set_long_mode(en, LV_LABEL_LONG_DOT);
        bind(s, s->cards[i], (unsigned)i);
    }
    lv_obj_add_event_cb(s->list, scrolled, LV_EVENT_SCROLL, s);
    sync(s);
    lv_obj_update_layout(s->list);
    if(!cfg->shuffle || !strcmp(cfg->theme_id, "face"))
        lv_obj_scroll_to_y(s->list, selected_index ? (int)(selected_index / 4) * ROW_PITCH - 6 : 0, LV_ANIM_OFF);
    lv_obj_t *fade = ui_box(s->root, 18, 666, 684, 32, 0xe9e9e9, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_hex(0xe9e9e9), 0);
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    return (ui_screen_t){s->root, s, update, destroy, paused};
}
