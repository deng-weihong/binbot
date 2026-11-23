#include "../ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name, *en, *icon; uint32_t color; } sounds[] = {
    {"雨声", "Rain", "rain", 0x7aa7d9}, {"海浪", "Waves", "wave", 0x5fb3c9},
    {"篝火", "Fire", "fire", 0xe8643c}, {"森林", "Forest", "tree", 0x7ab07a},
    {"咖啡馆", "Café", "cup", 0xb98a5e}, {"风声", "Wind", "fan", 0xa6b4c2},
    {"雷雨", "Storm", "storm", 0x6c6f9c}, {"溪流", "Stream", "drop", 0x62b7b0},
    {"键盘声", "Typing", "keys", 0x8f8a84},
};
static const struct { const char *name, *icon; uint8_t levels[9]; } presets[] = {
    {"专注", "focus", {55, 0, 0, 0, 35, 0, 0, 0, 0}},
    {"助眠", "moon", {0, 60, 0, 0, 0, 25, 0, 0, 0}},
    {"放松", "leaf", {0, 0, 0, 55, 0, 0, 0, 40, 0}},
    {"雨夜", "storm", {60, 0, 30, 0, 0, 0, 45, 0, 0}},
};
static const uint32_t durations[] = {900, 1800, 3600, 0};
typedef struct noise_context noise_context_t;
typedef struct { noise_context_t *context; unsigned index; } noise_binding_t;
struct noise_context {
    ui_app_t *app;
    lv_obj_t *root, *player, *status, *title, *play, *play_icon, *master, *speaker;
    lv_obj_t *badge, *badge_text, *badge_icon, *glows[2], *bars[14];
    lv_obj_t *tiles[9], *circles[9], *icons[9], *names[9], *english[9], *percent[9], *sliders[9];
    lv_obj_t *preset_buttons[4], *preset_labels[4], *preset_icons[4], *timer_buttons[4], *timer_labels[4];
    noise_binding_t voice_bindings[9], preset_bindings[4], timer_bindings[4];
    noise_binding_t master_binding;
    lv_timer_t *animation;
    uint32_t last_glow_tick;
    int32_t bar_center;
    bool paused;
};

static void tile_clicked(lv_event_t *event)
{
    noise_binding_t *b = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if(indev && lv_indev_get_press_moved(indev)) return;
    binbot_audio_noise_state_t next = b->context->app->noise;
    next.levels[b->index] = next.levels[b->index] ? 0 : 50;
    next.playing = 0;
    for(unsigned i = 0; i < 9; ++i) if(next.levels[i]) next.playing = 1;
    ui_noise_apply(b->context->app, &next);
}

static void preset_clicked(lv_event_t *event)
{
    noise_binding_t *b = lv_event_get_user_data(event);
    binbot_audio_noise_state_t next = b->context->app->noise;
    memcpy(next.levels, presets[b->index].levels, sizeof(next.levels));
    next.playing = 1;
    ui_noise_apply(b->context->app, &next);
}

static void timer_clicked(lv_event_t *event)
{
    noise_binding_t *b = lv_event_get_user_data(event);
    binbot_audio_noise_state_t next = b->context->app->noise;
    if(next.timer_seconds == durations[b->index]) return;
    next.remaining_seconds = next.timer_seconds = durations[b->index];
    ui_noise_apply(b->context->app, &next);
}

static void play_clicked(lv_event_t *event)
{
    noise_context_t *c = lv_event_get_user_data(event);
    binbot_audio_noise_state_t next = c->app->noise;
    next.playing = !next.playing;
    ui_noise_apply(c->app, &next);
}

static void volume_changed(lv_event_t *event)
{
    noise_binding_t *b = lv_event_get_user_data(event);
    binbot_audio_noise_state_t next = b->context->app->noise;
    uint8_t value = (uint8_t)lv_slider_get_value(lv_event_get_target(event));
    if(b->index < 9 && value < 5) value = 5;
    if(b->index == 9) next.master = value;
    else next.levels[b->index] = value;
    ui_noise_apply(b->context->app, &next);
}

static void slider_event(lv_event_t *event) { lv_event_stop_bubbling(event); }

static lv_obj_t *slider(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, x, y); lv_obj_set_size(s, w, h);
    lv_slider_set_range(s, 0, 100);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xf5f5f5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 8);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s, slider_event, LV_EVENT_ALL, NULL);
    return s;
}

static void animate(lv_timer_t *timer)
{
    noise_context_t *c = lv_timer_get_user_data(timer);
    if(c->paused || c->app->content_frozen) return;
    bool playing = c->app->noise.playing;
    uint32_t tick = lv_tick_get();
    for(unsigned i = 0; i < 14; ++i) {
        float phase = (float)((tick + (i * 137) % 900) % (900 + (i * 211) % 700)) /
                            (900 + (i * 211) % 700);
        int height = playing ? (int)lroundf(54 * (.625f - .375f * cosf(phase * 6.2831853f))) : 8;
        lv_obj_set_height(c->bars[i], height);
        lv_obj_set_y(c->bars[i], c->bar_center - height / 2);
    }
    if(playing && tick - c->last_glow_tick < 200) return;
    c->last_glow_tick = tick;
    for(unsigned i = 0; i < 2; ++i) {
        float opacity = i ? .15f : .2f;
        if(playing) opacity = .625f - .375f * cosf(tick * 6.2831853f / (i ? 7000 : 5000));
        lv_obj_set_style_bg_image_opa(c->glows[i], (lv_opa_t)(opacity * 255), 0);
    }
}

void ui_noise_screen_sync(void *user_data)
{
    noise_context_t *c = user_data;
    const binbot_audio_noise_state_t *state = &c->app->noise;
    unsigned active = 0, first[2] = {9, 9};
    char title[240] = "";
    for(unsigned i = 0; i < 9; ++i) {
        bool on = state->levels[i] > 0;
        if(on) {
            if(active < 2) first[active] = i;
            if(active++) strcat(title, " + ");
            strcat(title, sounds[i].name);
        }
        char surface[24]; snprintf(surface, sizeof(surface), "noise_on_%u", i);
        ui_ref_surface(c->tiles[i], on ? surface : "noise_off");
        ui_ref_shadow(c->tiles[i], 0, on ? 89 : 0, 52, -14, 12);
        lv_obj_set_style_bg_color(c->circles[i], lv_color_hex(on ? sounds[i].color : 0xffffff), 0);
        lv_obj_set_style_bg_opa(c->circles[i], on ? 255 : 179, 0);
        ui_icon_set_color(c->icons[i], on ? 0xffffff : 0x555555);
        lv_obj_set_y(c->names[i], on ? 107 : 109);
        char percent[12]; snprintf(percent, sizeof(percent), "%u%%", state->levels[i]);
        lv_label_set_text(c->percent[i], percent);
        if(on) {
            lv_obj_remove_flag(c->sliders[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(c->percent[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(c->english[i], LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(c->sliders[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(c->percent[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(c->english[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_slider_set_value(c->sliders[i], state->levels[i], LV_ANIM_OFF);
    }
    lv_label_set_text(c->status, state->playing ? "正在播放" : active ? "已暂停" : "选择声音开始");
    lv_label_set_text(c->title, active ? title : "安静一下");
    lv_obj_update_layout(c->title);
    int title_height = lv_obj_get_height(c->title);
    c->bar_center = (46 + (title_height > 62 ? title_height : 62) + 458) / 2;
    for(unsigned i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_image_recolor(c->glows[i], lv_color_hex(first[i] < 9 ? sounds[first[i]].color : 0xbdbdbd), 0);
        lv_obj_set_style_bg_image_recolor_opa(c->glows[i], LV_OPA_COVER, 0);
        if(i && first[i] == 9) lv_obj_add_flag(c->glows[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(c->glows[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(active) lv_obj_remove_state(c->play, LV_STATE_DISABLED);
    else lv_obj_add_state(c->play, LV_STATE_DISABLED);
    ui_ref_icon_set_name(c->play_icon, state->playing ? "pause" : "play");
    lv_slider_set_value(c->master, state->master, LV_ANIM_OFF);
    ui_icon_set_color(c->speaker, state->master > 15 ? UI_REF_INK : 0xffffff);
    for(unsigned p = 0; p < 4; ++p) {
        bool selected = true;
        for(unsigned i = 0; i < 9; ++i)
            if((state->levels[i] > 0) != (presets[p].levels[i] > 0)) selected = false;
        lv_obj_set_style_bg_image_opa(c->preset_buttons[p], selected ? 0 : 255, 0);
        lv_obj_set_style_bg_color(c->preset_buttons[p], lv_color_hex(selected ? UI_REF_CLAY : 0xffffff), 0);
        lv_obj_set_style_text_color(c->preset_labels[p], lv_color_hex(selected ? 0xffffff : 0x525252), 0);
        ui_icon_set_color(c->preset_icons[p], selected ? 0xffffff : 0x525252);
        ui_ref_shadow(c->preset_buttons[p], selected ? UI_REF_CLAY : 0, selected ? 230 : 31,
                      selected ? 20 : 30, selected ? -10 : -12, 10);
        selected = state->timer_seconds == durations[p];
        lv_obj_set_style_bg_color(c->timer_buttons[p], lv_color_hex(selected ? 0xffffff : 0x343434), 0);
        lv_obj_set_style_text_color(c->timer_labels[p], lv_color_hex(selected ? UI_REF_INK : 0xc6c6c6), 0);
    }
    char timer_text[48] = "不限时";
    if(state->timer_seconds) snprintf(timer_text, sizeof(timer_text), "%lu:%02lu 后停止",
        (unsigned long)(state->remaining_seconds / 60), (unsigned long)(state->remaining_seconds % 60));
    lv_label_set_text(c->badge_text, timer_text);
    lv_obj_update_layout(c->badge_text);
    int width = lv_obj_get_width(c->badge_text) + 62;
    lv_obj_set_width(c->badge, width);
    lv_obj_set_x(c->badge, 691 - width);
    animate(c->animation);
    if(state->playing && !c->paused) lv_timer_resume(c->animation);
    else lv_timer_pause(c->animation);
}

static void update(void *data, const app_model_t *model, uint32_t changes)
{ (void)model; (void)changes; ui_noise_screen_sync(data); }

static void paused(void *data, bool pause)
{
    noise_context_t *c = data;
    c->paused = pause;
    if(pause) lv_timer_pause(c->animation);
    else ui_noise_screen_sync(c);
}

static void destroy(void *data)
{
    noise_context_t *c = data;
    lv_timer_delete(c->animation);
    free(c);
}

ui_screen_t ui_noise_screen_create(ui_app_t *app)
{
    noise_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app;
    c->root = ui_ref_shell_latest(app, "白噪音", "WHITE NOISE");
    c->badge = ui_box(c->root, 584, 27, 107, 48, 0xffffff, 24);
    ui_ref_shadow(c->badge, 0, 31, 30, -12, 10);
    c->badge_icon = ui_ref_icon(c->badge, "timer", 18, 0x525252);
    lv_obj_set_pos(c->badge_icon, 18, 15);
    c->badge_text = ui_ref_label(c->badge, "不限时", 15, UI_FONT_REGULAR, 0x525252, 44, 13);
    c->player = ui_box(c->root, 22, 96, 262, 602, UI_REF_INK, 30);
    lv_obj_set_style_clip_corner(c->player, true, 0);
    for(unsigned i = 0; i < 2; ++i) {
        c->glows[i] = ui_box(c->player, 0, 0, 262, 602, 0, 30);
        lv_obj_set_style_bg_opa(c->glows[i], 0, 0);
        ui_ref_surface(c->glows[i], i ? "noise_bottom" : "noise_top");
    }
    c->status = ui_ref_label(c->player, "", 15, UI_FONT_REGULAR, 0x989898, 20, 20);
    c->title = ui_ref_label(c->player, "", 24, UI_FONT_SEMIBOLD, 0xffffff, 20, 46);
    lv_obj_set_width(c->title, 222);
    lv_label_set_long_mode(c->title, LV_LABEL_LONG_WRAP);
    for(unsigned i = 0; i < 14; ++i) c->bars[i] = ui_box(c->player, 64 + i * 10, 279, 5, 8, 0xd1d1d1, 3);
    c->play = ui_button(c->player, 20, 458, 68, 68, 0xffffff, 34);
    lv_obj_set_style_opa(c->play, 102, LV_STATE_DISABLED);
    c->play_icon = ui_ref_icon(c->play, "play", 28, UI_REF_INK); lv_obj_center(c->play_icon);
    lv_obj_add_event_cb(c->play, play_clicked, LV_EVENT_CLICKED, c);
    c->master = slider(c->player, 100, 468, 142, 48, 0xe1e1e1);
    lv_obj_set_style_bg_color(c->master, lv_color_hex(0x3c3c3c), LV_PART_MAIN);
    c->speaker = ui_ref_icon(c->master, "speaker", 18, UI_REF_INK);
    lv_obj_set_pos(c->speaker, 14, 15);
    static const char *times[] = {"15′", "30′", "60′", "∞"};
    for(unsigned i = 0; i < 4; ++i) {
        c->timer_bindings[i] = (noise_binding_t){c, i};
        c->timer_buttons[i] = ui_button(c->player, 20 + i * 57, 538, 51, 44, 0x343434, 22);
        c->timer_labels[i] = ui_centered_label(c->timer_buttons[i], times[i], ui_ref_font(15, UI_FONT_MEDIUM), 0xc6c6c6);
        lv_obj_add_event_cb(c->timer_buttons[i], timer_clicked, LV_EVENT_CLICKED, &c->timer_bindings[i]);
        c->preset_bindings[i] = (noise_binding_t){c, i};
        c->preset_buttons[i] = ui_ref_button(c->root, 298 + i * 102, 96, 94, 48, 24, "noise_preset");
        c->preset_icons[i] = ui_ref_icon(c->preset_buttons[i], presets[i].icon, 17, 0x525252);
        lv_obj_set_pos(c->preset_icons[i], 20, 16);
        c->preset_labels[i] = ui_ref_label(c->preset_buttons[i], presets[i].name, 15, UI_FONT_MEDIUM, 0x525252, 43, 13);
        lv_obj_add_event_cb(c->preset_buttons[i], preset_clicked, LV_EVENT_CLICKED, &c->preset_bindings[i]);
    }
    for(unsigned i = 0; i < 9; ++i) {
        unsigned col = i % 3, row = i / 3;
        c->voice_bindings[i] = (noise_binding_t){c, i};
        c->tiles[i] = ui_ref_button(c->root, 298 + (col * 410 + 1) / 3, 156 + row * 184, 127, 174, 24, "noise_off");
        lv_obj_add_event_cb(c->tiles[i], tile_clicked, LV_EVENT_CLICKED, &c->voice_bindings[i]);
        c->circles[i] = ui_box(c->tiles[i], 14, 14, 44, 44, 0xffffff, 22);
        c->icons[i] = ui_ref_icon(c->circles[i], sounds[i].icon, 21, 0x555555); lv_obj_center(c->icons[i]);
        c->percent[i] = ui_ref_label(c->tiles[i], "", 14, UI_FONT_REGULAR, UI_REF_FAINT, 67, 23);
        lv_obj_set_width(c->percent[i], 46);
        lv_obj_set_style_text_align(c->percent[i], LV_TEXT_ALIGN_RIGHT, 0);
        c->names[i] = ui_ref_label(c->tiles[i], sounds[i].name, 17, UI_FONT_SEMIBOLD, UI_REF_INK, 14, 109);
        c->english[i] = ui_ref_label(c->tiles[i], sounds[i].en, 14, UI_FONT_REGULAR, UI_REF_FAINT, 14, 133);
        c->sliders[i] = slider(c->tiles[i], 14, 144, 99, 8, sounds[i].color);
        lv_obj_add_event_cb(c->sliders[i], volume_changed, LV_EVENT_VALUE_CHANGED, &c->voice_bindings[i]);
    }
    c->master_binding = (noise_binding_t){c, 9};
    lv_obj_add_event_cb(c->master, volume_changed, LV_EVENT_VALUE_CHANGED, &c->master_binding);
    c->animation = lv_timer_create(animate, 50, c);
    ui_noise_screen_sync(c);
    return (ui_screen_t){c->root, c, update, destroy, paused};
}
