#include "ambient_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ambient_frame {
    binbot_ambient_display_t *display;
    lv_obj_t *root;
    const ambient_renderer_t *renderer;
    const ambient_theme_t *theme;
    void *context;
    ambient_frame_t *next;
};

static void copy_snapshot(binbot_ambient_display_t *display, const binbot_ambient_snapshot_t *snapshot)
{
    display->snapshot = *snapshot;
#define TERMINATE(field) display->snapshot.field[sizeof(display->snapshot.field) - 1] = 0
    TERMINATE(time); TERMINATE(date); TERMINATE(city); TERMINATE(weather);
    TERMINATE(weather_icon); TERMINATE(track); TERMINATE(artist); TERMINATE(next_todo);
    TERMINATE(weather_id); TERMINATE(weather_english);
    for(unsigned i = 0; i < 5; ++i) {
        TERMINATE(todos[i].text); TERMINATE(todos[i].time);
        TERMINATE(events[i].title); TERMINATE(events[i].time);
    }
    for(unsigned i = 0; i < 3; ++i) TERMINATE(world_time[i]);
    if(display->snapshot.event_count > 5) display->snapshot.event_count = 5;
#undef TERMINATE
}

static void dispose(void *user_data)
{
    ambient_frame_t *frame = user_data;
    ambient_frame_t **link = &frame->display->retired;
    while(*link && *link != frame) link = &(*link)->next;
    if(*link) *link = frame->next;
    lv_obj_delete(frame->root);
    if(frame->renderer->destroy) frame->renderer->destroy(frame->context);
    free(frame);
}

static void controls_show(binbot_ambient_display_t *display, bool visible)
{
    if(!display->controls) return;
    lv_obj_set_flag(display->controls, LV_OBJ_FLAG_HIDDEN, !visible);
    display->controls_at = lv_tick_get();
}

static void select_adjacent(binbot_ambient_display_t *display, int direction)
{
    if(!display->active || display->theme_count < 2) return;
    size_t index = (size_t)(display->active->theme - display->themes);
    index = direction < 0 ? (index <= 1 ? display->theme_count - 1 : index - 1) :
                           (index + 1 >= display->theme_count ? 1 : index + 1);
    const char *id = display->themes[index].info.id;
    if(binbot_ambient_show(display, id, &display->snapshot)) {
        controls_show(display, true);
        if(display->config.selected) display->config.selected(display->config.user_data, id);
    }
}

static void previous(lv_event_t *event) { select_adjacent(lv_event_get_user_data(event), -1); }
static void next(lv_event_t *event) { select_adjacent(lv_event_get_user_data(event), 1); }

static void wake_clicked(lv_event_t *event)
{
    binbot_ambient_display_t *display = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if(indev) lv_indev_wait_release(indev);
    if(display->config.wake) display->config.wake(display->config.user_data);
    else binbot_ambient_hide(display);
}

static void touch(lv_event_t *event)
{
    binbot_ambient_display_t *display = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_PRESSED) {
        if(indev) lv_indev_get_point(indev, &display->pressed_at);
        return;
    }
    if(code != LV_EVENT_RELEASED && code != LV_EVENT_CLICKED) return;
    if(!display->active || display->config.preview) return;
    if(!strcmp(display->active->theme->info.id, "face")) {
        if(code == LV_EVENT_CLICKED) wake_clicked(event);
        return;
    }
    if(code == LV_EVENT_CLICKED) { if(!indev) controls_show(display, true); return; }
    lv_point_t point = display->pressed_at;
    if(indev) lv_indev_get_point(indev, &point);
    int dx = point.x - display->pressed_at.x, dy = point.y - display->pressed_at.y;
    if(abs(dx) >= 64 && abs(dx) > abs(dy)) select_adjacent(display, dx > 0 ? -1 : 1);
    else controls_show(display, !display->controls || lv_obj_has_flag(display->controls, LV_OBJ_FLAG_HIDDEN));
}

static lv_obj_t *control_button(binbot_ambient_display_t *display, lv_obj_t *parent,
                                 const char *text, int x, int width, lv_event_cb_t callback)
{
    bool exit = callback == wake_clicked;
    lv_obj_t *button = ambient_box(parent, x, 0, width, 56, 0xffffff, 28);
    lv_obj_set_style_bg_opa(button, exit ? 255 : 38, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, display);
    lv_obj_t *label = ambient_text(display, button, text, exit ? 17 : 28, 600, exit ? 0x1a1a1a : 0xffffff, 0, 0);
    lv_obj_center(label);
    return button;
}

static void create_controls(binbot_ambient_display_t *display, ambient_frame_t *frame)
{
    if(display->config.preview || !strcmp(frame->theme->info.id, "face")) return;
    lv_obj_t *controls = ambient_box(frame->root, 0, 0, 720, 720, 0, 0);
    lv_obj_set_style_bg_opa(controls, 0, 0);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *top = ambient_box(controls, 0, 0, 720, 144, 0, 0);
    lv_obj_set_style_bg_grad_color(top, lv_color_hex(0), 0);
    lv_obj_set_style_bg_grad_dir(top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(top, 178, 0);lv_obj_set_style_bg_grad_opa(top, 0, 0);
    lv_obj_t *fade = ambient_box(controls, 0, 560, 720, 160, 0, 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_hex(0), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, 0, 0);lv_obj_set_style_bg_grad_opa(fade, 191, 0);
    lv_obj_t *english = ambient_text(display, controls, frame->theme->info.english, 15, 400, 0xffffff, 0, 24);
    lv_obj_set_width(english, 720);lv_obj_set_style_text_align(english, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(english, 3, 0);lv_obj_set_style_opa(english, 153, 0);
    lv_obj_t *name = ambient_text(display, controls, frame->theme->info.name, 22, 600, 0xffffff, 0, 45);
    lv_obj_set_width(name, 720);lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    unsigned count = (unsigned)display->theme_count - 1, selected = (unsigned)(frame->theme - display->themes) - 1;
    int x = (720 - (int)(count * 6 + (count - 1) * 8 + 18)) / 2;
    for(unsigned i = 0; i < count; ++i) {
        int width = i == selected ? 24 : 6;
        lv_obj_t *dot = ambient_box(controls, x, 84, width, 6, 0xffffff, 3);
        lv_obj_set_style_bg_opa(dot, i == selected ? 255 : 102, 0);x += width + 8;
    }
    lv_obj_t *bottom = ambient_box(controls, 228, 640, 264, 56, 0, 0);
    lv_obj_set_style_bg_opa(bottom, 0, 0);
    control_button(display, bottom, "‹", 0, 56, previous);
    control_button(display, bottom, "退出待机", 70, 124, wake_clicked);
    control_button(display, bottom, "›", 208, 56, next);
    display->controls = controls;
    controls_show(display, true);
}

static void tick(lv_timer_t *timer)
{
    binbot_ambient_display_t *display = lv_timer_get_user_data(timer);
    if(display->active && display->active->renderer->tick)
        display->active->renderer->tick(display->active->context, lv_tick_get());
    if(display->controls && lv_tick_elaps(display->controls_at) >= 3200) controls_show(display, false);
}

static void entry_y(void *object, int32_t value) { lv_obj_set_y(object, value); }
static void entry_opacity(void *object, int32_t value) { lv_obj_set_style_opa(object, value, 0); }

binbot_ambient_display_t *binbot_ambient_create(const binbot_ambient_config_t *config)
{
    if(!config || !config->parent) return NULL;
    binbot_ambient_display_t *display = calloc(1, sizeof(*display));
    if(!display) return NULL;
    display->config = *config;
    const char *root = config->asset_root;
    if(!root || !*root) root = "/oem/assets";
    if(strlen(root) >= sizeof(display->asset_root)) { free(display); return NULL; }
    snprintf(display->asset_root, sizeof(display->asset_root), "%s", root);
    display->config.asset_root = display->asset_root;
    (void)ambient_registry_load(display);
    display->timer = lv_timer_create(tick, 33, display);
    if(!display->timer) { free(display); return NULL; }
    lv_timer_pause(display->timer);
    return display;
}

bool binbot_ambient_reload(binbot_ambient_display_t *display)
{
    if(!display || display->active || display->retired) return false;
    return ambient_registry_load(display);
}

size_t binbot_ambient_theme_count(const binbot_ambient_display_t *display)
{
    return display ? display->theme_count : 0;
}

const binbot_ambient_theme_t *binbot_ambient_theme_at(const binbot_ambient_display_t *display, size_t index)
{
    return display && index < display->theme_count ? &display->themes[index].info : NULL;
}

const binbot_ambient_theme_t *binbot_ambient_theme_find(const binbot_ambient_display_t *display, const char *id)
{
    if(!display || !id) return NULL;
    for(size_t i = 0; i < display->theme_count; ++i)
        if(!strcmp(display->themes[i].info.id, id)) return &display->themes[i].info;
    return NULL;
}

void binbot_ambient_hide(binbot_ambient_display_t *display)
{
    if(!display || !display->active) return;
    ambient_frame_t *frame = display->active;
    display->active = NULL;
    display->controls = NULL;
    lv_timer_pause(display->timer);
    lv_anim_delete(frame->root, NULL);
    lv_obj_add_flag(frame->root, LV_OBJ_FLAG_HIDDEN);
    frame->next = display->retired;
    display->retired = frame;
    if(lv_async_call(dispose, frame) != LV_RESULT_OK) dispose(frame);
}

bool binbot_ambient_show(binbot_ambient_display_t *display, const char *theme_id,
                         const binbot_ambient_snapshot_t *snapshot)
{
    if(!display) return false;
    if(snapshot) copy_snapshot(display, snapshot);
    const ambient_theme_t *theme = &display->themes[0];
    for(size_t i = 0; theme_id && i < display->theme_count; ++i)
        if(!strcmp(display->themes[i].info.id, theme_id)) { theme = &display->themes[i]; break; }
    if(display->active && display->active->theme == theme) {
        binbot_ambient_update(display, &display->snapshot);
        binbot_ambient_raise(display);
        return true;
    }
    binbot_ambient_hide(display);
    ambient_frame_t *frame = calloc(1, sizeof(*frame));
    if(!frame) return false;
    frame->display = display;
    frame->theme = theme;
    frame->renderer = ambient_renderer_find(theme->renderer);
    frame->root = ambient_box(display->config.parent, 0, 0, 720, 720, theme->info.background, 0);
    if(!frame->root) { free(frame); return false; }
    if(!display->config.preview) {
        lv_obj_add_flag(frame->root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(frame->root, touch, LV_EVENT_PRESSED, display);
        lv_obj_add_event_cb(frame->root, touch, LV_EVENT_RELEASED, display);
        lv_obj_add_event_cb(frame->root, touch, LV_EVENT_CLICKED, display);
    }
    frame->context = frame->renderer->create(display, frame->root, theme);
    if(!frame->context) {
        lv_obj_delete(frame->root);
        free(frame);
        if(theme != &display->themes[0]) return binbot_ambient_show(display, "face", snapshot);
        return false;
    }
    display->active = frame;
    frame->renderer->update(frame->context, &display->snapshot);
    if(frame->renderer->tick) frame->renderer->tick(frame->context, lv_tick_get() + (display->config.preview ? 1800 : 0));
    create_controls(display, frame);
    if(display->config.preview) return true;
    lv_timer_set_period(display->timer, theme->refresh_ms);
    lv_timer_resume(display->timer);
    lv_timer_reset(display->timer);
    lv_anim_t entry;
    lv_anim_init(&entry);
    lv_anim_set_var(&entry, frame->root);
    lv_anim_set_duration(&entry, 350);
    lv_anim_set_path_cb(&entry, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&entry, entry_y);
    lv_anim_set_values(&entry, 12, 0);
    lv_anim_start(&entry);
    lv_anim_set_exec_cb(&entry, entry_opacity);
    lv_anim_set_values(&entry, 0, 255);
    lv_anim_start(&entry);
    return true;
}

void binbot_ambient_update(binbot_ambient_display_t *display, const binbot_ambient_snapshot_t *snapshot)
{
    if(!display || !snapshot) return;
    copy_snapshot(display, snapshot);
    if(display->active) display->active->renderer->update(display->active->context, &display->snapshot);
}

void binbot_ambient_raise(binbot_ambient_display_t *display)
{
    if(display && display->active) lv_obj_move_foreground(display->active->root);
}

bool binbot_ambient_active(const binbot_ambient_display_t *display)
{
    return display && display->active;
}

const char *binbot_ambient_current_theme(const binbot_ambient_display_t *display)
{
    return display && display->active ? display->active->theme->info.id : NULL;
}

void binbot_ambient_destroy(binbot_ambient_display_t *display)
{
    if(!display) return;
    binbot_ambient_hide(display);
    lv_timer_delete(display->timer);
    while(display->retired) {
        ambient_frame_t *frame = display->retired;
        lv_async_call_cancel(dispose, frame);
        dispose(frame);
    }
    free(display);
}

