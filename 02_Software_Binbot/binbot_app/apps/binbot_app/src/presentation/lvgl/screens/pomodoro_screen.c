#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct pomodoro_context pomodoro_context_t;
typedef struct { pomodoro_context_t *context; uint32_t minutes; } preset_binding_t;

struct pomodoro_context {
    ui_app_t *app;
    lv_obj_t *arc;
    lv_obj_t *time;
    lv_obj_t *status;
    lv_obj_t *total;
    lv_obj_t *play_icon;
    lv_obj_t *play_label;
    lv_obj_t *presets[4];
    lv_obj_t *preset_labels[4];
    preset_binding_t bindings[4];
};

static void preset_clicked(lv_event_t *event)
{
    preset_binding_t *binding = lv_event_get_user_data(event);
    app_state_set_pomodoro_duration(binding->context->app->state, binding->minutes);
}

static void duration_step(lv_event_t *event)
{
    pomodoro_context_t *context = lv_event_get_user_data(event);
    const app_model_t *model = app_state_model(context->app->state);
    int minutes = (int)(model->pomodoro.focus_seconds / 60);
    minutes += (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    if(minutes < 5) minutes = 5;
    if(minutes > 120) minutes = 120;
    app_state_set_pomodoro_duration(context->app->state, (uint32_t)minutes);
}

static void play_clicked(lv_event_t *event)
{
    pomodoro_context_t *context = lv_event_get_user_data(event);
    if(app_state_model(context->app->state)->pomodoro.running)
        app_state_pause_pomodoro(context->app->state);
    else app_state_start_pomodoro(context->app->state);
}

static void reset_clicked(lv_event_t *event)
{
    pomodoro_context_t *context = lv_event_get_user_data(event);
    app_state_reset_pomodoro(context->app->state);
}

static void noise_clicked(lv_event_t *event)
{
    pomodoro_context_t *context = lv_event_get_user_data(event);
    ui_app_navigate(context->app, UI_ROUTE_NOISE);
}

static void pomodoro_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    pomodoro_context_t *context = user_data;
    if(changes != APP_CHANGE_ALL &&
       !(changes & (APP_CHANGE_POMODORO | APP_CHANGE_POMODORO_PROGRESS))) return;
    const app_pomodoro_t *timer = &model->pomodoro;
    char text[48];
    snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(timer->seconds_left / 60),
             (unsigned long)(timer->seconds_left % 60));
    lv_label_set_text(context->time, text);
    lv_label_set_text(context->status, timer->mode == APP_POMODORO_BREAK
                      ? (timer->running ? "休息中" : "准备休息")
                      : (timer->running ? "专注中" : "准备开始"));
    uint32_t total = timer->mode == APP_POMODORO_FOCUS
                         ? timer->focus_seconds : APP_POMODORO_BREAK_SECONDS;
    snprintf(text, sizeof(text), "共 %lu 分钟", (unsigned long)(total / 60));
    lv_label_set_text(context->total, text);
    lv_arc_set_range(context->arc, 0, (int32_t)total);
    lv_arc_set_value(context->arc, (int32_t)timer->seconds_left);
    ui_ref_icon_set_name(context->play_icon, timer->running ? "pause" : "play");
    lv_label_set_text(context->play_label, timer->running ? "暂停" : "开始");
    for(size_t i = 0; i < 4; ++i) {
        bool selected = timer->focus_seconds == context->bindings[i].minutes * 60;
        lv_obj_set_style_bg_image_opa(context->presets[i], selected ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(context->presets[i], lv_color_hex(selected ? UI_REF_INK : 0xe7e7e7), 0);
        lv_obj_set_style_text_color(context->preset_labels[i], lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
    }
}

static void pomodoro_destroy(void *user_data) { free(user_data); }

ui_screen_t ui_pomodoro_screen_create(ui_app_t *app)
{
    pomodoro_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    lv_obj_t *root = ui_ref_shell_latest(app, "专注模式", "FOCUS");
    lv_obj_t *dial = ui_box(root, 207, 96, 306, 306, 0xffffff, 153);
    ui_ref_surface(dial, "focus_dial");
    context->arc = ui_ref_ring(dial, 13, 13, 280, 10);
    lv_obj_set_style_arc_color(context->arc, lv_color_hex(0xececec), LV_PART_MAIN);
    context->status = ui_ref_label(dial, "", 12, UI_FONT_REGULAR, UI_REF_FAINT, 0, 99);
    context->time = ui_ref_label(dial, "", 68, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 112);
    context->total = ui_ref_label(dial, "", 12, UI_FONT_REGULAR, UI_REF_FAINT, 0, 191);
    lv_obj_t *labels[] = {context->status, context->time, context->total};
    for(size_t i = 0; i < 3; ++i) {
        lv_obj_set_width(labels[i], 306);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    static const uint32_t minutes[] = {15, 25, 45, 60};
    for(size_t i = 0; i < 4; ++i) {
        context->presets[i] = ui_ref_button(root, 119 + (int32_t)i * 90, 427, 79, 43, 22, "focus_preset");
        char text[24];
        snprintf(text, sizeof(text), "%lu 分", (unsigned long)minutes[i]);
        context->preset_labels[i] = ui_centered_label(context->presets[i], text, ui_ref_font(13, UI_FONT_MEDIUM), UI_REF_INK);
        context->bindings[i] = (preset_binding_t){context, minutes[i]};
        lv_obj_add_event_cb(context->presets[i], preset_clicked, LV_EVENT_CLICKED, &context->bindings[i]);
    }
    lv_obj_t *custom = ui_box(root, 479, 427, 122, 43, 0xe7e7e7, 22);
    ui_ref_surface(custom, "focus_custom");
    ui_ref_label(custom, "自定", 12, UI_FONT_REGULAR, UI_REF_MUTED, 49, 12);
    for(int i = 0; i < 2; ++i) {
        lv_obj_t *step = ui_button(custom, i ? 79 : 0, 0, 43, 43, 0xe7e7e7, 22);
        lv_obj_set_style_bg_opa(step, LV_OPA_TRANSP, 0);
        ui_centered_label(step, i ? "+" : "−", ui_ref_font(18, UI_FONT_REGULAR), UI_REF_INK);
        lv_obj_set_user_data(step, (void *)(intptr_t)(i ? 5 : -5));
        lv_obj_add_event_cb(step, duration_step, LV_EVENT_CLICKED, context);
    }
    lv_obj_t *reset = ui_ref_button(root, 252, 495, 65, 65, 33, "focus_reset");
    lv_obj_t *stop = ui_ref_icon(reset, "stop", 23, UI_REF_INK);
    lv_obj_center(stop);
    lv_obj_add_event_cb(reset, reset_clicked, LV_EVENT_CLICKED, context);
    lv_obj_t *play = ui_button(root, 335, 495, 133, 65, UI_REF_CLAY, 33);
    ui_ref_shadow(play, UI_REF_CLAY, 255, 56, -8, 12);
    context->play_icon = ui_ref_icon(play, "play", 23, 0xffffff);
    lv_obj_set_pos(context->play_icon, 36, 21);
    context->play_label = ui_ref_label(play, "开始", 15, UI_FONT_SEMIBOLD, 0xffffff, 66, 21);
    lv_obj_add_event_cb(play, play_clicked, LV_EVENT_CLICKED, context);
    lv_obj_t *noise = ui_button(root, 265, 604, 190, 54, 0xffffff, 27);
    lv_obj_t *wave = ui_ref_icon(noise, "wave", 21, UI_REF_INK);
    lv_obj_set_pos(wave, 42, 17);
    ui_ref_label(noise, "白噪音", 17, UI_FONT_MEDIUM, UI_REF_INK, 75, 14);
    lv_obj_add_event_cb(noise, noise_clicked, LV_EVENT_CLICKED, context);
    return (ui_screen_t){root, context, pomodoro_update, pomodoro_destroy, NULL};
}
