#include "../ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct alarm_context alarm_context_t;
typedef struct { alarm_context_t *context; size_t index; } alarm_binding_t;

struct alarm_context {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *arc;
    lv_obj_t *now_dot;
    lv_obj_t *next_time;
    lv_obj_t *next_label;
    lv_obj_t *until;
    lv_obj_t *trial;
    lv_obj_t *rows[APP_ALARM_CAPACITY];
    lv_obj_t *times[APP_ALARM_CAPACITY];
    lv_obj_t *details[APP_ALARM_CAPACITY];
    lv_obj_t *switches[APP_ALARM_CAPACITY];
    lv_obj_t *dots[APP_ALARM_CAPACITY];
    lv_point_precise_t ticks[24][2];
    alarm_binding_t bindings[APP_ALARM_CAPACITY];
    size_t row_count;
    size_t next_index;
    lv_timer_t *timer;
    lv_obj_t *overlay;
    lv_obj_t *hours;
    lv_obj_t *minutes;
    lv_obj_t *repeat;
    lv_obj_t *days[7];
    lv_obj_t *labels[4];
    size_t edit_index;
    app_alarm_t edit;
};

static const char *const weekdays[] = {"一", "二", "三", "四", "五", "六", "日"};
static const char *const alarm_labels[] = {"起床", "午休结束", "运动", "该睡觉了"};
static void open_editor(alarm_context_t *context, size_t index);

static int alarm_minute(const app_alarm_t *alarm)
{
    return ((alarm->time[0] - '0') * 10 + alarm->time[1] - '0') * 60 +
           (alarm->time[3] - '0') * 10 + alarm->time[4] - '0';
}

static lv_point_precise_t dial_point(int minute, int radius)
{
    double angle = minute * (2.0 * 3.141592653589793 / 1440.0);
    return (lv_point_precise_t){180 + (int32_t)lround(radius * sin(angle)),
                               180 - (int32_t)lround(radius * cos(angle))};
}

static time_t next_occurrence(const app_alarm_t *alarm, time_t now)
{
    struct tm current;
    localtime_r(&now, &current);
    for(int offset = 0; offset < 8; ++offset) {
        struct tm date = current;
        date.tm_mday += offset;
        date.tm_hour = alarm_minute(alarm) / 60;
        date.tm_min = alarm_minute(alarm) % 60;
        date.tm_sec = 0;
        date.tm_isdst = -1;
        time_t candidate = mktime(&date);
        unsigned weekday = (unsigned)(date.tm_wday + 6) % 7;
        if(candidate > now && (!alarm->days || (alarm->days & (1U << weekday))))
            return candidate;
    }
    return 0;
}

static void update_clock(alarm_context_t *context)
{
    const app_model_t *model = app_state_model(context->app->state);
    time_t now = time(NULL), next = 0;
    context->next_index = APP_ALARM_CAPACITY;
    for(size_t i = 0; i < model->alarm_count; ++i) {
        if(!model->alarms[i].enabled) continue;
        time_t candidate = next_occurrence(&model->alarms[i], now);
        if(candidate && (!next || candidate < next)) {
            next = candidate;
            context->next_index = i;
        }
    }
    struct tm date;
    localtime_r(&now, &date);
    int current_minute = date.tm_hour * 60 + date.tm_min;
    lv_point_precise_t point = dial_point(current_minute, 150);
    lv_obj_set_pos(context->now_dot, point.x - 6, point.y - 6);
    char text[96];
    if(next) {
        const app_alarm_t *alarm = &model->alarms[context->next_index];
        snprintf(text, sizeof(text), "下一个 · %s", alarm->label);
        lv_label_set_text(context->next_label, text);
        lv_label_set_text(context->next_time, alarm->time);
        long minutes = (long)((next - now + 30) / 60);
        snprintf(text, sizeof(text), "%ld 小时 %ld 分钟后", minutes / 60, minutes % 60);
        lv_label_set_text(context->until, text);
        lv_obj_remove_flag(context->trial, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_opa(context->arc, 64, LV_PART_INDICATOR);
        lv_arc_set_start_angle(context->arc, current_minute / 4);
        lv_arc_set_end_angle(context->arc, alarm_minute(alarm) / 4);
    }
    else {
        lv_label_set_text(context->next_label, "没有开启的闹钟");
        lv_label_set_text(context->next_time, "--:--");
        lv_label_set_text(context->until, "");
        lv_obj_add_flag(context->trial, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_opa(context->arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }
}

static void clock_tick(lv_timer_t *timer)
{
    alarm_context_t *context = lv_timer_get_user_data(timer);
    update_clock(context);
}

static void alarm_toggled(lv_event_t *event)
{
    alarm_binding_t *binding = lv_event_get_user_data(event);
    app_state_toggle_alarm(binding->context->app->state, binding->index);
}

static void alarm_clicked(lv_event_t *event)
{
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    alarm_binding_t *binding = lv_event_get_user_data(event);
    open_editor(binding->context, binding->index);
}

static void add_alarm_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    size_t count = app_state_model(context->app->state)->alarm_count;
    if(count == APP_ALARM_CAPACITY) {
        ui_app_show_toast(context->app, "最多可设置 8 个闹钟");
        return;
    }
    open_editor(context, count);
}

static void close_overlay(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    if(context->overlay) lv_obj_delete_async(context->overlay);
    context->overlay = NULL;
}

static void editor_sync(alarm_context_t *context)
{
    char text[96], repeat[48];
    app_state_format_alarm_repeat(context->edit.days, repeat, sizeof(repeat));
    snprintf(text, sizeof(text), "重复 · %s", repeat);
    lv_label_set_text(context->repeat, text);
    for(size_t i = 0; i < 7; ++i) {
        bool selected = context->edit.days & (1U << i);
        lv_obj_set_style_bg_color(context->days[i], lv_color_hex(selected ? UI_REF_INK : 0xe7e7e7), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(context->days[i], 0),
                                    lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
    }
    for(size_t i = 0; i < 4; ++i) {
        bool selected = strcmp(context->edit.label, alarm_labels[i]) == 0;
        lv_obj_set_style_bg_color(context->labels[i], lv_color_hex(selected ? UI_REF_CLAY : 0xe7e7e7), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(context->labels[i], 0),
                                    lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
    }
}

static void day_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    context->edit.days ^= (uint8_t)(1U << index);
    editor_sync(context);
}

static void label_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    snprintf(context->edit.label, sizeof(context->edit.label), "%s", alarm_labels[index]);
    editor_sync(context);
}

static void save_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    char text[6];
    snprintf(text, sizeof(text), "%02u:%02u", lv_roller_get_selected(context->hours),
             lv_roller_get_selected(context->minutes));
    if(app_state_save_alarm(context->app->state, context->edit_index, text,
                              context->edit.label, context->edit.days, context->edit.enabled))
        ui_app_rebuild(context->app);
}

static void delete_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    if(app_state_delete_alarm(context->app->state, context->edit_index))
        ui_app_rebuild(context->app);
}

static lv_obj_t *time_roller(lv_obj_t *parent, int32_t x, unsigned count, unsigned selected)
{
    char options[192];
    size_t length = 0;
    for(unsigned i = 0; i < count; ++i)
        length += (size_t)snprintf(options + length, sizeof(options) - length,
                                    i + 1 == count ? "%02u" : "%02u\n", i);
    lv_obj_t *roller = lv_roller_create(parent);
    lv_obj_remove_style_all(roller);
    const lv_font_t *font = ui_ref_font(60, UI_FONT_SEMIBOLD);
    ui_resources_set_font(roller, font, LV_PART_MAIN);
    ui_resources_set_font(roller, font, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(UI_REF_FAINT), LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_hex(UI_REF_INK), LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(roller, 84 - lv_font_get_line_height(font), LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_SELECTED);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_pos(roller, x, 110);
    lv_obj_set_size(roller, 120, 420);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    return roller;
}

static void open_editor(alarm_context_t *context, size_t index)
{
    if(context->overlay) return;
    const app_model_t *model = app_state_model(context->app->state);
    bool is_new = index == model->alarm_count;
    context->edit_index = index;
    context->edit = is_new ? (app_alarm_t){.time = "07:30", .label = "闹钟", .days = 0x1f, .enabled = true}
                           : model->alarms[index];
    context->overlay = ui_box(context->root, 0, 0, 720, 720, 0xefefef, 0);
    lv_obj_add_flag(context->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->overlay, close_overlay, LV_EVENT_CLICKED, context);
    lv_obj_set_style_bg_opa(context->overlay, LV_OPA_TRANSP, 0);
    ui_ref_backdrop(context->overlay, context->root, 0xefefef, 153, 12);
    lv_obj_set_style_bg_opa(context->overlay, LV_OPA_COVER, 0);
    lv_obj_t *panel = ui_box(context->overlay, 20, 20, 680, 680, 0xffffff, 36);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    ui_ref_surface(panel, "alarm_editor");
    lv_obj_t *wheel = ui_box(panel, 24, 24, 330, 632, 0xe7e7e7, 28);
    ui_ref_surface(wheel, "alarm_wheel");
    lv_obj_t *selection = ui_box(wheel, 16, 274, 298, 84, 0xffffff, 22);
    ui_ref_shadow(selection, 0x000000, 30, 3, 0, 1);
    int minute = alarm_minute(&context->edit);
    context->hours = time_roller(wheel, 30, 24, (unsigned)minute / 60);
    context->minutes = time_roller(wheel, 184, 60, (unsigned)minute % 60);
    ui_ref_label(wheel, ":", 56, UI_FONT_SEMIBOLD, 0xd4d4d4, 157, 272);
    lv_obj_t *fade = ui_box(wheel, 0, 106, 330, 420, 0xffffff, 0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_TRANSP, 0);
    ui_ref_surface(fade, "alarm_wheel_fade");
    ui_ref_label(panel, is_new ? "新建闹钟" : "编辑闹钟", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 378, 32);
    lv_obj_t *close = ui_button(panel, 608, 24, 48, 48, 0xe7e7e7, 24);
    lv_obj_t *icon = ui_ref_icon(close, "x", 20, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(close, close_overlay, LV_EVENT_CLICKED, context);
    context->repeat = ui_ref_label(panel, "", 16, UI_FONT_REGULAR, UI_REF_FAINT, 378, 94);
    lv_obj_set_width(context->repeat, 278);
    lv_label_set_long_mode(context->repeat, LV_LABEL_LONG_DOT);
    for(size_t i = 0; i < 7; ++i) {
        lv_obj_t *button = ui_button(panel, 378 + (int32_t)(i % 4) * 72, 124 + (int32_t)(i / 4) * 60,
                                     64, 52, 0xe7e7e7, 26);
        context->days[i] = button;
        ui_centered_label(button, weekdays[i], ui_ref_font(18, UI_FONT_MEDIUM), UI_REF_INK);
        lv_obj_set_user_data(button, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(button, day_clicked, LV_EVENT_CLICKED, context);
    }
    ui_ref_label(panel, "标签", 16, UI_FONT_REGULAR, UI_REF_FAINT, 378, 259);
    for(size_t i = 0; i < 4; ++i) {
        lv_obj_t *button = ui_button(panel, 378 + (int32_t)(i % 2) * 143, 288 + (int32_t)(i / 2) * 56,
                                     135, 48, 0xe7e7e7, 24);
        context->labels[i] = button;
        ui_centered_label(button, alarm_labels[i], ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
        lv_obj_set_user_data(button, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(button, label_clicked, LV_EVENT_CLICKED, context);
    }
    if(!is_new) {
        lv_obj_t *del = ui_button(panel, 378, 592, 64, 64, 0xe7e7e7, 32);
        icon = ui_ref_icon(del, "trash", 23, UI_REF_INK);
        lv_obj_center(icon);
        lv_obj_add_event_cb(del, delete_clicked, LV_EVENT_CLICKED, context);
    }
    lv_obj_t *save = ui_button(panel, is_new ? 378 : 454, 592, is_new ? 278 : 202, 64, UI_REF_CLAY, 32);
    ui_ref_shadow(save, UI_REF_CLAY, 255, 56, -8, 12);
    ui_centered_label(save, "保存", ui_ref_font(19, UI_FONT_SEMIBOLD), 0xffffff);
    lv_obj_add_event_cb(save, save_clicked, LV_EVENT_CLICKED, context);
    editor_sync(context);
}

static void trial_clicked(lv_event_t *event)
{
    alarm_context_t *context = lv_event_get_user_data(event);
    if(context->overlay || context->next_index >= APP_ALARM_CAPACITY) return;
    const app_alarm_t *alarm = &app_state_model(context->app->state)->alarms[context->next_index];
    context->overlay = ui_box(context->root, 0, 0, 720, 720, 0xefefef, 0);
    ui_ref_surface(context->overlay, "alarm_ringing");
    lv_obj_add_flag(context->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *pulse = ui_box(context->overlay, 230, 77, 260, 260, UI_REF_CLAY, 130);
    lv_obj_set_style_bg_opa(pulse, 30, 0);
    pulse = ui_box(context->overlay, 272, 119, 176, 176, UI_REF_CLAY, 88);
    lv_obj_set_style_bg_opa(pulse, 102, 0);
    lv_obj_t *bell = ui_box(context->overlay, 296, 143, 128, 128, UI_REF_CLAY, 64);
    ui_ref_shadow(bell, UI_REF_CLAY, 255, 80, -12, 20);
    lv_obj_t *icon = ui_ref_icon(bell, "alarm", 56, 0xffffff);
    lv_obj_center(icon);
    lv_obj_t *time = ui_ref_label(context->overlay, alarm->time, 110, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 318);
    lv_obj_set_width(time, 720);
    lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *label = ui_ref_label(context->overlay, alarm->label, 24, UI_FONT_MEDIUM, UI_REF_INK, 0, 451);
    lv_obj_set_width(label, 720);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *snooze = ui_button(context->overlay, 165, 525, 238, 76, 0xffffff, 38);
    ui_centered_label(snooze, "稍后提醒 · 10 分钟", ui_ref_font(20, UI_FONT_SEMIBOLD), UI_REF_INK);
    lv_obj_add_event_cb(snooze, close_overlay, LV_EVENT_CLICKED, context);
    lv_obj_t *close = ui_button(context->overlay, 419, 525, 136, 76, UI_REF_INK, 38);
    ui_centered_label(close, "关闭", ui_ref_font(20, UI_FONT_SEMIBOLD), 0xffffff);
    lv_obj_add_event_cb(close, close_overlay, LV_EVENT_CLICKED, context);
}

static void alarm_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    alarm_context_t *context = user_data;
    if(changes != APP_CHANGE_ALL && !(changes & APP_CHANGE_ALARMS)) return;
    if(context->row_count != model->alarm_count) {
        ui_app_rebuild(context->app);
        return;
    }
    for(size_t i = 0; i < model->alarm_count; ++i) {
        const app_alarm_t *alarm = &model->alarms[i];
        ui_ref_surface(context->rows[i], alarm->enabled ? "alarm_row_latest" : "alarm_row_off");
        lv_obj_set_style_shadow_opa(context->rows[i], alarm->enabled ? 31 : 0, 0);
        lv_obj_set_style_text_color(context->times[i], lv_color_hex(alarm->enabled ? UI_REF_INK : UI_REF_FAINT), 0);
        lv_obj_set_style_text_color(context->details[i], lv_color_hex(alarm->enabled ? UI_REF_MUTED : UI_REF_FAINT), 0);
        lv_label_set_text(context->times[i], alarm->time);
        char text[96];
        snprintf(text, sizeof(text), "%s · %s", alarm->label, alarm->repeat);
        lv_label_set_text(context->details[i], text);
        if(alarm->enabled) lv_obj_add_state(context->switches[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(context->switches[i], LV_STATE_CHECKED);
        int size = alarm->enabled ? 25 : 19;
        lv_point_precise_t point = dial_point(alarm_minute(alarm), 150);
        lv_obj_set_pos(context->dots[i], point.x - size / 2, point.y - size / 2);
        lv_obj_set_size(context->dots[i], size, size);
        lv_obj_set_style_bg_color(context->dots[i], lv_color_hex(alarm->enabled ? UI_REF_CLAY : 0xffffff), 0);
        lv_obj_set_style_border_color(context->dots[i], lv_color_hex(alarm->enabled ? 0xffffff : 0xc4c4c4), 0);
    }
    update_clock(context);
}

static void alarm_paused(void *user_data, bool paused)
{
    alarm_context_t *context = user_data;
    if(paused) lv_timer_pause(context->timer);
    else { lv_timer_resume(context->timer); update_clock(context); }
}

static void alarm_destroy(void *user_data)
{
    alarm_context_t *context = user_data;
    lv_timer_delete(context->timer);
    free(context);
}

ui_screen_t ui_alarm_screen_create(ui_app_t *app)
{
    alarm_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->root = ui_ref_shell_latest(app, "闹钟", "ALARM");
    lv_obj_t *root = context->root;
    lv_obj_t *add = ui_button(root, 581, 25, 110, 56, UI_REF_INK, 28);
    lv_obj_t *plus = ui_ref_icon(add, "plus", 20, 0xffffff);
    lv_obj_set_pos(plus, 24, 18);
    ui_ref_label(add, "新建", 17, UI_FONT_MEDIUM, 0xffffff, 52, 15);
    lv_obj_add_event_cb(add, add_alarm_clicked, LV_EVENT_CLICKED, context);
    lv_obj_t *dial = ui_box(root, 20, 99, 400, 601, 0xffffff, 36);
    ui_ref_surface(dial, "alarm_dial");
    lv_obj_t *clock = ui_box(dial, 20, 120, 360, 360, 0xffffff, 0);
    lv_obj_set_style_bg_opa(clock, LV_OPA_TRANSP, 0);
    context->arc = ui_ref_ring(clock, 19, 19, 322, 22);
    lv_obj_set_style_arc_color(context->arc, lv_color_hex(0xefefef), LV_PART_MAIN);
    for(int i = 0; i < 24; ++i) {
        context->ticks[i][0] = dial_point(i * 60, 128);
        context->ticks[i][1] = dial_point(i * 60, i % 6 ? 123 : 118);
        lv_obj_t *line = lv_line_create(clock);
        lv_line_set_points(line, context->ticks[i], 2);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(i % 6 ? 0xd4d4d4 : UI_REF_FAINT), 0);
    }
    for(int i = 0; i < 4; ++i) {
        char text[4];
        snprintf(text, sizeof(text), "%d", i * 6);
        lv_point_precise_t point = dial_point(i * 360, 174);
        lv_obj_t *label = ui_ref_label(clock, text, 15, UI_FONT_REGULAR, UI_REF_FAINT, point.x - 15, point.y - 10);
        lv_obj_set_width(label, 30);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    context->now_dot = ui_box(clock, 0, 0, 12, 12, 0x1f1f1f, 6);
    context->next_label = ui_ref_label(dial, "", 16, UI_FONT_REGULAR, UI_REF_FAINT, 25, 241);
    context->next_time = ui_ref_label(dial, "", 64, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 260);
    context->until = ui_ref_label(dial, "", 16, UI_FONT_MEDIUM, UI_REF_CLAY, 0, 340);
    lv_obj_set_width(context->next_label, 350);
    lv_label_set_long_mode(context->next_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(context->next_time, 400);
    lv_obj_set_width(context->until, 400);
    lv_obj_set_style_text_align(context->next_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(context->next_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(context->until, LV_TEXT_ALIGN_CENTER, 0);
    context->trial = ui_button(dial, 160, 533, 80, 48, 0xe7e7e7, 24);
    ui_centered_label(context->trial, "试响", ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(context->trial, trial_clicked, LV_EVENT_CLICKED, context);

    lv_obj_t *list = ui_box(root, 436, 99, 264, 601, 0xefefef, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const app_model_t *model = app_state_model(app->state);
    context->row_count = model->alarm_count;
    size_t order[APP_ALARM_CAPACITY];
    for(size_t i = 0; i < model->alarm_count; ++i) order[i] = i;
    for(size_t i = 1; i < model->alarm_count; ++i) {
        size_t value = order[i], j = i;
        while(j && strcmp(model->alarms[order[j - 1]].time, model->alarms[value].time) > 0) {
            order[j] = order[j - 1]; --j;
        }
        order[j] = value;
    }
    for(size_t row = 0; row < model->alarm_count; ++row) {
        size_t i = order[row];
        context->bindings[i] = (alarm_binding_t){context, i};
        context->rows[i] = ui_button(list, 0, (int32_t)row * 109, 264, 97, 0xffffff, 26);
        lv_obj_add_event_cb(context->rows[i], alarm_clicked, LV_EVENT_CLICKED, &context->bindings[i]);
        context->times[i] = ui_ref_label(context->rows[i], "", 36, UI_FONT_SEMIBOLD, UI_REF_INK, 20, 11);
        context->details[i] = ui_ref_label(context->rows[i], "", 15, UI_FONT_REGULAR, UI_REF_MUTED, 20, 61);
        lv_obj_set_width(context->details[i], 150);
        lv_label_set_long_mode(context->details[i], LV_LABEL_LONG_DOT);
        lv_obj_t *toggle = lv_switch_create(context->rows[i]);
        lv_obj_set_pos(toggle, 182, 30);
        lv_obj_set_size(toggle, 62, 36);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0xd6d6d6), LV_PART_MAIN);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(UI_REF_CLAY), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(toggle, -4, LV_PART_KNOB);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0xffffff), LV_PART_KNOB);
        lv_obj_remove_flag(toggle, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(toggle, alarm_toggled, LV_EVENT_VALUE_CHANGED, &context->bindings[i]);
        context->switches[i] = toggle;
        context->dots[i] = ui_button(clock, 0, 0, 25, 25, UI_REF_CLAY, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(context->dots[i], 3, 0);
        lv_obj_add_event_cb(context->dots[i], alarm_clicked, LV_EVENT_CLICKED, &context->bindings[i]);
    }
    context->timer = lv_timer_create(clock_tick, 30000, context);
    return (ui_screen_t){root, context, alarm_update, alarm_destroy, alarm_paused};
}
