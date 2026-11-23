#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct todo_context todo_context_t;
typedef struct { todo_context_t *context; size_t index; } todo_binding_t;

static const char *const time_keys[] = {
    "1", "2", "3", LV_SYMBOL_CLOSE, "\n",
    "4", "5", "6", LV_SYMBOL_OK, "\n",
    "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
    ":", "0", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "",
};
static const lv_buttonmatrix_ctrl_t time_key_controls[] = {
    1, 1, 1, 2,
    1, 1, 1, 2,
    1, 1, 1, 2,
    1, 1, 1, 1,
};

struct todo_context {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *summary;
    lv_obj_t *ring;
    lv_obj_t *percent;
    lv_obj_t *entry;
    lv_obj_t *textarea;
    lv_obj_t *time;
    lv_obj_t *keyboard;
    lv_obj_t *list;
    lv_obj_t *new_label;
    lv_obj_t *new_icon;
    lv_obj_t *rows[APP_TODO_CAPACITY];
    lv_obj_t *checks[APP_TODO_CAPACITY];
    lv_obj_t *texts[APP_TODO_CAPACITY];
    lv_obj_t *details[APP_TODO_CAPACITY];
    todo_binding_t bindings[APP_TODO_CAPACITY];
    size_t row_count;
    bool adding;
};

static void todo_toggled(lv_event_t *event)
{
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    todo_binding_t *binding = lv_event_get_user_data(event);
    app_state_toggle_todo(binding->context->app->state, binding->index);
}

static void todo_deleted(lv_event_t *event)
{
    todo_binding_t *binding = lv_event_get_user_data(event);
    if(app_state_delete_todo(binding->context->app->state, binding->index))
        ui_app_rebuild(binding->context->app);
}

static void todo_keyboard_event(lv_event_t *event)
{
    todo_context_t *context = lv_event_get_user_data(event);
    if(lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(context->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(context->keyboard, NULL);
    }
}

static void todo_input_clicked(lv_event_t *event)
{
    todo_context_t *context = lv_event_get_user_data(event);
    lv_obj_t *input = lv_event_get_current_target(event);
    lv_keyboard_set_mode(context->keyboard, input == context->time
                                              ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(context->keyboard, input);
    lv_obj_remove_flag(context->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(context->keyboard);
}

static void todo_show_entry(lv_event_t *event)
{
    todo_context_t *context = lv_event_get_user_data(event);
    context->adding = !context->adding;
    if(context->adding) lv_obj_remove_flag(context->entry, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_add_flag(context->entry, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(context->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(context->keyboard, NULL);
    }
    lv_obj_set_y(context->list, context->adding ? 276 : 208);
    lv_obj_set_height(context->list, context->adding ? 422 : 490);
    lv_label_set_text(context->new_label, context->adding ? "取消" : "新建");
    ui_ref_icon_set_name(context->new_icon, context->adding ? "x" : "plus");
}

static void todo_add(lv_event_t *event)
{
    todo_context_t *context = lv_event_get_user_data(event);
    const char *text = lv_textarea_get_text(context->textarea);
    if(!text || !text[0]) return;
    if(!app_state_add_todo_at_time(context->app->state, text, lv_textarea_get_text(context->time))) {
        ui_app_show_toast(context->app, "请检查时间或待办数量");
        return;
    }
    ui_app_rebuild(context->app);
}

static void todo_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    todo_context_t *context = user_data;
    if(changes != APP_CHANGE_ALL && !(changes & APP_CHANGE_TODOS)) return;
    if(context->row_count != model->todo_count) {
        ui_app_rebuild(context->app);
        return;
    }
    size_t done = 0;
    for(size_t i = 0; i < model->todo_count; ++i) if(model->todos[i].done) ++done;
    unsigned percent = model->todo_count ? (unsigned)(done * 100 / model->todo_count) : 0;
    char text[64];
    snprintf(text, sizeof(text), "已完成 %lu / %lu 项", (unsigned long)done,
             (unsigned long)model->todo_count);
    lv_label_set_text(context->summary, text);
    snprintf(text, sizeof(text), "%u%%", percent);
    lv_label_set_text(context->percent, text);
    lv_arc_set_value(context->ring, (int32_t)percent);
    for(size_t i = 0; i < context->row_count; ++i) {
        const app_todo_t *task = &model->todos[i];
        ui_icon_set_visible(context->checks[i], task->done);
        lv_obj_t *check = lv_obj_get_parent(context->checks[i]);
        lv_obj_set_style_bg_image_opa(check, task->done ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(check, lv_color_hex(task->done ? UI_REF_INK : 0xe7e7e7), 0);
        lv_obj_set_style_opa(context->rows[i], task->done ? 140 : LV_OPA_COVER, 0);
        lv_obj_set_style_text_decor(context->texts[i],
                                    task->done ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
        lv_label_set_text(context->texts[i], task->text);
        lv_label_set_text(context->details[i], task->time[0] ? task->time : task->tag);
    }
}

static void todo_destroy(void *user_data) { free(user_data); }

static lv_obj_t *todo_input(todo_context_t *context, int32_t x, int32_t width)
{
    lv_obj_t *input = lv_textarea_create(context->entry);
    lv_obj_remove_style_all(input);
    lv_obj_set_pos(input, x, 7);
    lv_textarea_set_one_line(input, true);
    ui_resources_set_font(input, ui_ref_font(14, UI_FONT_REGULAR), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(UI_REF_INK), 0);
    lv_obj_set_style_pad_top(input, 12, 0);
    lv_obj_set_size(input, width, 43);
    lv_obj_add_event_cb(input, todo_input_clicked, LV_EVENT_CLICKED, context);
    return input;
}

ui_screen_t ui_todo_screen_create(ui_app_t *app)
{
    todo_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->root = ui_ref_shell(app, "提醒与待办", "TODAY");
    lv_obj_t *root = context->root;
    lv_obj_t *new_button = ui_button(root, 596, 25, 95, 50, UI_REF_INK, 25);
    context->new_icon = ui_ref_icon(new_button, "plus", 18, 0xffffff);
    lv_obj_set_pos(context->new_icon, 22, 16);
    context->new_label = ui_ref_label(new_button, "新建", 13, UI_FONT_MEDIUM, 0xffffff, 47, 15);
    lv_obj_add_event_cb(new_button, todo_show_entry, LV_EVENT_CLICKED, context);

    lv_obj_t *summary = ui_box(root, 22, 93, 676, 101, 0xe7e7e7, 27);
    ui_ref_surface(summary, "todo_summary");
    context->ring = ui_ref_ring(summary, 18, 18, 65, 7);
    context->percent = ui_ref_label(summary, "", 13, UI_FONT_SEMIBOLD, UI_REF_INK, 18, 40);
    lv_obj_set_width(context->percent, 65);
    lv_obj_set_style_text_align(context->percent, LV_TEXT_ALIGN_CENTER, 0);
    context->summary = ui_ref_label(summary, "", 18, UI_FONT_SEMIBOLD, UI_REF_INK, 101, 27);
    ui_ref_label(summary, "点击任务标记完成，保持节奏", 12, UI_FONT_REGULAR,
                  UI_REF_MUTED, 101, 55);

    context->entry = ui_box(root, 22, 205, 676, 57, 0xffffff, 23);
    ui_ref_surface(context->entry, "todo_entry");
    context->textarea = todo_input(context, 18, 414);
    lv_textarea_set_placeholder_text(context->textarea, "提醒内容…");
    lv_textarea_set_max_length(context->textarea, 71);
    context->time = todo_input(context, 446, 112);
    lv_textarea_set_accepted_chars(context->time, "0123456789:");
    lv_textarea_set_max_length(context->time, 5);
    lv_textarea_set_text(context->time, "18:00");
    lv_obj_set_style_bg_color(context->time, lv_color_hex(0xe7e7e7), 0);
    lv_obj_set_style_bg_opa(context->time, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(context->time, 22, 0);
    lv_obj_set_style_pad_left(context->time, 16, 0);
    lv_obj_t *add = ui_button(context->entry, 576, 7, 93, 43, UI_REF_CLAY, 22);
    ui_centered_label(add, "添加", ui_ref_font(14, UI_FONT_MEDIUM), 0xffffff);
    lv_obj_add_event_cb(add, todo_add, LV_EVENT_CLICKED, context);
    lv_obj_add_flag(context->entry, LV_OBJ_FLAG_HIDDEN);

    context->list = ui_box(root, 22, 208, 676, 490, 0, 0);
    lv_obj_set_style_bg_opa(context->list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(context->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->list, LV_SCROLLBAR_MODE_OFF);
    const app_model_t *model = app_state_model(app->state);
    context->row_count = model->todo_count;
    for(size_t i = 0; i < context->row_count; ++i) {
        context->bindings[i] = (todo_binding_t){context, i};
        lv_obj_t *row = ui_ref_button(context->list, 0, (int32_t)i * 79, 676, 68, 23, "todo_row");
        context->rows[i] = row;
        lv_obj_add_event_cb(row, todo_toggled, LV_EVENT_CLICKED, &context->bindings[i]);
        lv_obj_t *check = ui_ref_button(row, 11, 9, 50, 50, 25, "soft50");
        context->checks[i] = ui_ref_icon(check, "check", 20, 0xffffff);
        lv_obj_center(context->checks[i]);
        lv_obj_add_event_cb(check, todo_toggled, LV_EVENT_CLICKED, &context->bindings[i]);
        context->texts[i] = ui_ref_label(row, "", 15, UI_FONT_MEDIUM, UI_REF_INK, 75, 13);
        lv_obj_set_width(context->texts[i], 535);
        lv_label_set_long_mode(context->texts[i], LV_LABEL_LONG_DOT);
        context->details[i] = ui_ref_label(row, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 75, 39);
        lv_obj_t *remove = ui_button(row, 622, 13, 43, 43, 0, 22);
        lv_obj_set_style_bg_opa(remove, LV_OPA_TRANSP, 0);
        lv_obj_t *trash = ui_ref_icon(remove, "trash", 18, UI_REF_FAINT);
        lv_obj_center(trash);
        lv_obj_add_event_cb(remove, todo_deleted, LV_EVENT_CLICKED, &context->bindings[i]);
    }
    if(!context->row_count)
        ui_ref_label(context->list, "暂无待办", 15, UI_FONT_REGULAR, UI_REF_FAINT, 306, 80);

    context->keyboard = lv_keyboard_create(root);
    lv_keyboard_set_map(context->keyboard, LV_KEYBOARD_MODE_NUMBER, time_keys, time_key_controls);
    lv_obj_set_size(context->keyboard, UI_WIDTH, 290);
    lv_obj_align(context->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(context->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(context->keyboard, todo_keyboard_event, LV_EVENT_READY, context);
    lv_obj_add_event_cb(context->keyboard, todo_keyboard_event, LV_EVENT_CANCEL, context);
    todo_update(context, model, APP_CHANGE_ALL);
    return (ui_screen_t){root, context, todo_update, todo_destroy, NULL};
}
