#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct music_context music_context_t;
typedef struct { music_context_t *context; size_t index; } music_binding_t;
struct music_context {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *cover;
    lv_obj_t *cover_index;
    lv_obj_t *cover_caption;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *slider;
    lv_obj_t *elapsed;
    lv_obj_t *duration;
    lv_obj_t *play_icon;
    lv_obj_t *like_button;
    lv_obj_t *like_icon;
    lv_obj_t *repeat_icon;
    lv_obj_t *repeat_one;
    lv_obj_t *playlist;
    lv_obj_t *backdrop;
    lv_obj_t *rows[UI_MUSIC_TRACK_COUNT];
    lv_obj_t *indexes[UI_MUSIC_TRACK_COUNT];
    lv_obj_t *titles[UI_MUSIC_TRACK_COUNT];
    lv_obj_t *artists[UI_MUSIC_TRACK_COUNT];
    music_binding_t bindings[UI_MUSIC_TRACK_COUNT];
};

static void format_time(char *text, size_t size, uint32_t seconds)
{
    snprintf(text, size, "%02lu:%02lu", (unsigned long)(seconds / 60U),
             (unsigned long)(seconds % 60U));
}

static void music_back(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_app_go_back(context->app);
}

static void music_play(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_toggle_playback(context->app);
}

static void music_previous(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_previous(context->app);
}

static void music_next(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_next(context->app);
}

static void music_like(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_toggle_like(context->app);
}

static void music_repeat(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_toggle_repeat(context->app);
}

static void music_seek(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    ui_music_seek(context->app, (uint32_t)lv_slider_get_value(context->slider));
}

static void music_list_show(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    if(context->backdrop) lv_obj_delete(context->backdrop);
    context->backdrop = ui_ref_backdrop(context->playlist, context->root, 0xefefef, LV_OPA_60, 12);
    lv_obj_remove_flag(context->playlist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(context->playlist);
}

static void music_list_close(lv_event_t *event)
{
    music_context_t *context = lv_event_get_user_data(event);
    if(lv_event_get_current_target(event) == context->playlist &&
       lv_event_get_target(event) != context->playlist) return;
    lv_obj_add_flag(context->playlist, LV_OBJ_FLAG_HIDDEN);
}

static void music_select(lv_event_t *event)
{
    music_binding_t *binding = lv_event_get_user_data(event);
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    ui_music_select_track(binding->context->app, binding->index);
    if(!binding->context->app->music.playing)
        ui_music_toggle_playback(binding->context->app);
    lv_obj_add_flag(binding->context->playlist, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *music_button(music_context_t *context, lv_obj_t *parent, int32_t x,
                               int32_t y, int32_t size, const char *name,
                               int32_t icon_size, const char *surface,
                               lv_event_cb_t callback, lv_obj_t **icon_out)
{
    lv_obj_t *button = ui_ref_button(parent, x, y, size, size, size / 2, surface);
    if(!surface) lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = ui_ref_icon(button, name, icon_size, UI_REF_INK);
    lv_obj_center(icon);
    if(callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    if(icon_out) *icon_out = icon;
    return button;
}

static void music_create_playlist(music_context_t *context)
{
    context->playlist = ui_box(context->root, 0, 0, 720, 720, 0xefefef, 0);
    lv_obj_set_style_bg_opa(context->playlist, LV_OPA_60, 0);
    lv_obj_add_flag(context->playlist, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->playlist, music_list_close, LV_EVENT_CLICKED, context);
    lv_obj_t *panel = ui_box(context->playlist, 22, 414, 676, 284, 0xffffff, 32);
    ui_ref_surface(panel, "playlist");
    ui_ref_label(panel, "播放列表", 16, UI_FONT_SEMIBOLD, UI_REF_INK, 25, 28);
    music_button(context, panel, 611, 18, 40, "x", 16, "soft40", music_list_close, NULL);
    for(size_t i = 0; i < ui_music_track_count(); ++i) {
        const ui_music_track_t *track = ui_music_track_at(i);
        context->bindings[i] = (music_binding_t){context, i};
        lv_obj_t *row = ui_ref_button(panel, 18, 69 + (int32_t)i * 68, 640, 61, 20, "playlist_row");
        context->rows[i] = row;
        lv_obj_add_event_cb(row, music_select, LV_EVENT_CLICKED, &context->bindings[i]);
        char index[12];
        snprintf(index, sizeof(index), "%02lu", (unsigned long)i + 1);
        context->indexes[i] = ui_ref_label(row, index, 13, UI_FONT_SEMIBOLD, UI_REF_FAINT, 14, 21);
        context->titles[i] = ui_ref_label(row, track->title, 14, UI_FONT_MEDIUM, UI_REF_INK, 64, 10);
        context->artists[i] = ui_ref_label(row, track->artist, 11, UI_FONT_REGULAR, UI_REF_FAINT, 64, 33);
        lv_obj_set_width(context->titles[i], 510);
        lv_label_set_long_mode(context->titles[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(context->artists[i], 510);
        lv_label_set_long_mode(context->artists[i], LV_LABEL_LONG_DOT);
    }
    lv_obj_add_flag(context->playlist, LV_OBJ_FLAG_HIDDEN);
}

void ui_music_screen_sync(void *user_data)
{
    music_context_t *context = user_data;
    if(!context || !context->app) return;
    const ui_music_state_t *state = &context->app->music;
    const ui_music_track_t *track = ui_music_current_track(context->app);
    lv_label_set_text(context->title, track->title);
    lv_label_set_text(context->artist, track->artist);
    lv_label_set_text(context->status, state->playing ? "正在播放" : "已暂停");
    char text[128];
    snprintf(text, sizeof(text), "%02lu", (unsigned long)state->track_index + 1);
    lv_label_set_text(context->cover_index, text);
    snprintf(text, sizeof(text), "VOL. %02lu · %s", (unsigned long)state->track_index + 1, track->title);
    lv_label_set_text(context->cover_caption, text);
    static const char *covers[] = {"cover0", "cover1", "cover2"};
    ui_ref_surface(context->cover, covers[state->track_index % 3]);
    ui_ref_icon_set_name(context->play_icon, state->playing ? "pause" : "play");
    ui_ref_icon_set_name(context->like_icon, state->liked ? "heart_filled" : "heart");
    lv_obj_set_style_bg_image_opa(context->like_button, state->liked ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(context->like_button, lv_color_hex(state->liked ? UI_REF_CLAY : 0xffffff), 0);
    ui_icon_set_color(context->like_icon, state->liked ? 0xffffff : UI_REF_INK);
    ui_icon_set_color(context->repeat_icon, state->repeating ? UI_REF_CLAY : UI_REF_MUTED);
    if(state->repeating) lv_obj_remove_flag(context->repeat_one, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(context->repeat_one, LV_OBJ_FLAG_HIDDEN);
    format_time(text, sizeof(text), state->position_seconds);
    lv_label_set_text(context->elapsed, text);
    format_time(text, sizeof(text), track->duration_seconds);
    lv_label_set_text(context->duration, text);
    lv_slider_set_range(context->slider, 0, (int32_t)track->duration_seconds);
    lv_slider_set_value(context->slider, (int32_t)state->position_seconds, LV_ANIM_OFF);
    for(size_t i = 0; i < ui_music_track_count(); ++i) {
        bool selected = i == state->track_index;
        lv_obj_set_style_bg_image_opa(context->rows[i], selected ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(context->rows[i], lv_color_hex(selected ? UI_REF_INK : 0xe7e7e7), 0);
        lv_obj_set_style_text_color(context->indexes[i], lv_color_hex(selected ? UI_REF_CLAY : UI_REF_FAINT), 0);
        lv_obj_set_style_text_color(context->titles[i], lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
        lv_obj_set_style_text_color(context->artists[i], lv_color_hex(selected ? 0xaaaaaa : UI_REF_FAINT), 0);
    }
}

static void music_update(void *context, const app_model_t *model, uint32_t changes)
{
    (void)context; (void)model; (void)changes;
}
static void music_destroy(void *context) { free(context); }

ui_screen_t ui_music_screen_create(ui_app_t *app)
{
    music_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->root = ui_ref_page(app);
    music_button(context, context->root, 29, 25, 50, "back", 22, "circle50", music_back, NULL);
    music_button(context, context->root, 641, 25, 50, "more", 22, "circle50", music_list_show, NULL);
    context->status = ui_ref_label(context->root, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 100, 27);
    lv_obj_set_width(context->status, 520);
    lv_obj_set_style_text_align(context->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *heading = ui_ref_label(context->root, "音乐", 18, UI_FONT_SEMIBOLD, UI_REF_INK, 100, 45);
    lv_obj_set_width(heading, 520);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    context->cover = ui_box(context->root, 207, 86, 306, 306, 0x303030, 36);
    ui_ref_surface(context->cover, "cover0");
    lv_obj_t *badge = ui_box(context->cover, 22, 22, 43, 43, 0, 22);
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_40, 0);
    context->cover_index = ui_centered_label(badge, "01", ui_ref_font(12, UI_FONT_SEMIBOLD), 0xffffff);
    ui_ref_label(context->cover, "SLOW RADIO", 11, UI_FONT_REGULAR, 0x999999, 25, 244);
    context->cover_caption = ui_ref_label(context->cover, "", 14, UI_FONT_SEMIBOLD, 0xffffff, 25, 269);
    lv_obj_set_width(context->cover_caption, 256);
    lv_label_set_long_mode(context->cover_caption, LV_LABEL_LONG_DOT);
    context->title = ui_ref_label(context->root, "", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 108, 419);
    lv_obj_set_width(context->title, 433);
    lv_label_set_long_mode(context->title, LV_LABEL_LONG_DOT);
    context->artist = ui_ref_label(context->root, "", 13, UI_FONT_REGULAR, UI_REF_MUTED, 108, 450);
    lv_obj_set_width(context->artist, 433);
    lv_label_set_long_mode(context->artist, LV_LABEL_LONG_DOT);
    context->like_button = music_button(context, context->root, 562, 417, 50, "heart", 22,
                                         "circle50", music_like, &context->like_icon);

    context->slider = lv_slider_create(context->root);
    lv_obj_remove_style_all(context->slider);
    lv_obj_set_pos(context->slider, 108, 497);
    lv_obj_set_size(context->slider, 504, 5);
    lv_obj_set_style_bg_color(context->slider, lv_color_hex(0xdcdcdc), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(context->slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(context->slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(context->slider, lv_color_hex(UI_REF_CLAY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(context->slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(context->slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_width(context->slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(context->slider, 18, LV_PART_KNOB);
    lv_obj_set_style_pad_all(context->slider, 6, LV_PART_KNOB);
    lv_obj_set_style_bg_color(context->slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(context->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(context->slider, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(context->slider, lv_color_hex(UI_REF_CLAY), LV_PART_KNOB);
    lv_obj_set_style_radius(context->slider, 9, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(context->slider, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(context->slider, 64, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(context->slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(context->slider, 2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(context->slider, 14);
    lv_obj_add_event_cb(context->slider, music_seek, LV_EVENT_VALUE_CHANGED, context);
    context->elapsed = ui_ref_label(context->root, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 108, 515);
    context->duration = ui_ref_label(context->root, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 558, 515);
    lv_obj_set_width(context->duration, 54);
    lv_obj_set_style_text_align(context->duration, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *repeat = music_button(context, context->root, 108, 559, 50, "repeat", 24,
                                     NULL, music_repeat, &context->repeat_icon);
    context->repeat_one = ui_centered_label(repeat, "1", ui_ref_font(11, UI_FONT_BOLD), UI_REF_CLAY);
    music_button(context, context->root, 213, 555, 58, "prev", 25, NULL, music_previous, NULL);
    lv_obj_t *play = music_button(context, context->root, 320, 545, 79, "play", 32, NULL,
                                   music_play, &context->play_icon);
    lv_obj_set_style_bg_color(play, lv_color_hex(UI_REF_CLAY), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(play, lv_color_hex(UI_REF_CLAY), 0);
    lv_obj_set_style_shadow_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(play, 32, 0);
    lv_obj_set_style_shadow_spread(play, -10, 0);
    lv_obj_set_style_shadow_offset_y(play, 16, 0);
    ui_icon_set_color(context->play_icon, 0xffffff);
    music_button(context, context->root, 448, 555, 58, "next", 25, NULL, music_next, NULL);
    music_button(context, context->root, 562, 559, 50, "playlist", 24, NULL, music_list_show, NULL);
    music_create_playlist(context);
    ui_music_screen_sync(context);
    return (ui_screen_t){context->root, context, music_update, music_destroy, NULL};
}
