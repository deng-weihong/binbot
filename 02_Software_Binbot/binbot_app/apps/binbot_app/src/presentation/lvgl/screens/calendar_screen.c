#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { CAL_HOUR = 68, CAL_GRID_WIDTH = 656, CAL_DAY_HEIGHT = 46 };

typedef struct calendar_context calendar_context_t;
typedef struct {
    calendar_context_t *context;
    time_t day;
} calendar_day_binding_t;
typedef struct {
    calendar_context_t *context;
    char id[64];
} calendar_event_binding_t;

struct calendar_context {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *track;
    lv_obj_t *scroller;
    lv_obj_t *now_line;
    calendar_day_binding_t days[128];
    calendar_event_binding_t events[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
    uint32_t generation;
    time_t minute;
    lv_point_t press;
    bool tracking;
    bool dragged;
    bool animating;
    bool detail;
    int shift;
    int axis;
};

static const char *const weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static lv_obj_t *calendar_label(lv_obj_t *parent, const char *text, unsigned size,
                                ui_font_weight_t weight, uint32_t color,
                                int x, int y, int line_height)
{
    const lv_font_t *font = ui_ref_font(size, weight);
    return ui_label(parent, text, font, color, x, y + (line_height - font->line_height) / 2);
}

static lv_obj_t *calendar_card(lv_obj_t *parent, int x, int y, int w, int h, int radius, bool soft)
{
    lv_obj_t *card = ui_box(parent, x, y, w, h, soft ? 0xececec : 0xffffff, radius);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(soft ? 0xe2e2e2 : 0xf6f6f6), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    if(!soft) ui_ref_shadow(card, 0, 31, 30, -12, 10);
    return card;
}

static lv_obj_t *calendar_button(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *button = ui_button(parent, x, y, w, h, color, radius);
    lv_obj_set_style_transform_scale(button, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    return button;
}

static lv_obj_t *calendar_icon(lv_obj_t *parent, const char *name, int size, uint32_t color, int x, int y)
{
    lv_obj_t *icon = ui_ref_icon(parent, name, size, color);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

static void date_text(char *text, size_t capacity, time_t value, bool year)
{
    struct tm tm;
    localtime_r(&value, &tm);
    if(year) snprintf(text, capacity, "%d年%d月%d日 %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, weekdays[tm.tm_wday]);
    else snprintf(text, capacity, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, weekdays[tm.tm_wday]);
}

static void hours_text(char *text, size_t capacity, time_t value)
{
    struct tm tm;
    localtime_r(&value, &tm);
    snprintf(text, capacity, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static bool accepted_click(calendar_context_t *context, lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    return context && !context->app->route_pending && !context->animating &&
           (!indev || !lv_indev_get_press_moved(indev));
}

static void select_day(calendar_context_t *context, time_t day)
{
    if(day == (time_t)-1) return;
    context->app->calendar_day = app_calendar_day(day);
    context->app->calendar_scroll_valid = false;
    ui_app_rebuild(context->app);
}

static void day_clicked(lv_event_t *event)
{
    calendar_day_binding_t *binding = lv_event_get_user_data(event);
    if(!binding->context->dragged && accepted_click(binding->context, event)) select_day(binding->context, binding->day);
}

static void today_clicked(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    if(accepted_click(context, event)) select_day(context, time(NULL));
}

static void period_clicked(lv_event_t *event)
{
    calendar_day_binding_t *binding = lv_event_get_user_data(event);
    if(accepted_click(binding->context, event)) select_day(binding->context, binding->day);
}

static void fold_clicked(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    if(!accepted_click(context, event)) return;
    context->app->calendar_folded = !context->app->calendar_folded;
    ui_app_rebuild(context->app);
}

void ui_calendar_open_event(ui_app_t *app, const char *id, bool from_home)
{
    if(!app || !id) return;
    snprintf(app->calendar_selected, sizeof(app->calendar_selected), "%s", id);
    app->calendar_from_home = from_home;
    app->calendar_detail_scroll_y = 0;
    ui_app_navigate(app, UI_ROUTE_CALENDAR_DETAIL);
}

static void event_clicked(lv_event_t *event)
{
    calendar_event_binding_t *binding = lv_event_get_user_data(event);
    calendar_context_t *context = binding->context;
    if(context->dragged || !accepted_click(context, event)) return;
    if(context->scroller) {
        context->app->calendar_scroll_y = lv_obj_get_scroll_y(context->scroller);
        context->app->calendar_scroll_valid = true;
    }
    ui_calendar_open_event(context->app, binding->id, false);
}

static void back_clicked(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    if(!accepted_click(context, event)) return;
    if(!context->detail && context->scroller) {
        context->app->calendar_scroll_y = lv_obj_get_scroll_y(context->scroller);
        context->app->calendar_scroll_valid = true;
    }
    ui_app_go_back(context->app);
}

static void sync_clicked(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    const app_calendar_t *calendar = &context->app->calendar;
    char text[96];
    if(calendar->received) {
        char received[64];
        app_calendar_when(received, sizeof(received), calendar->received_at, time(NULL));
        snprintf(text, sizeof(text), "上次接收日程：%s", received);
    }
    else snprintf(text, sizeof(text), "%s", calendar->connected ? "正在等待飞书日程" : "暂时无法连接日程服务");
    ui_app_show_toast(context->app, text);
}

static void header(calendar_context_t *context)
{
    lv_obj_t *root = context->root;
    lv_obj_t *back = ui_ref_button(root, 29, 26, 50, 50, 25, "circle50");
    lv_obj_t *icon = ui_ref_icon(back, "back", 25, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, context);
    calendar_label(root, context->detail ? "EVENT" : "CALENDAR", 15, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27, 22);
    calendar_label(root, context->detail ? "日程详情" : "日程", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 49, 30);
    if(context->detail) {
        lv_obj_t *source = calendar_card(root, 571, 27, 120, 48, 24, false);
        lv_obj_t *brand = ui_box(source, 16, 14, 20, 20, 0x3370ff, 5);
        ui_centered_label(brand, "飞", ui_ref_font(11, UI_FONT_SEMIBOLD), 0xffffff);
        calendar_label(source, "飞书日历", 15, UI_FONT_REGULAR, 0x525252, 44, 13, 22);
        return;
    }
    const app_calendar_t *calendar = &context->app->calendar;
    bool offline = calendar->connection_known && !calendar->connected;
    const char *status = offline ? "离线" : calendar->received ? "已同步" : "同步中";
    const char *name = offline ? "cloudOff" : calendar->received ? "check" : "sync";
    int width = offline ? 87 : 102;
    lv_obj_t *sync = calendar_card(root, 691 - width, 27, width, 48, 24, false);
    lv_obj_add_flag(sync, LV_OBJ_FLAG_CLICKABLE);
    uint32_t color = !offline && calendar->received ? 0x009966 : UI_REF_MUTED;
    calendar_icon(sync, name, 19, color, 14, 14);
    calendar_label(sync, status, 15, UI_FONT_MEDIUM, color, 41, 13, 22);
    lv_obj_add_event_cb(sync, sync_clicked, LV_EVENT_CLICKED, context);
    bool today = context->app->calendar_day == app_calendar_day(time(NULL));
    lv_obj_t *button = calendar_button(root, 691 - width - 80, 27, 66, 48,
                                       today ? 0xe7e7e7 : UI_REF_INK, 24);
    ui_centered_label(button, "今天", ui_ref_font(15, UI_FONT_MEDIUM), today ? UI_REF_FAINT : 0xffffff);
    if(today) lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, today_clicked, LV_EVENT_CLICKED, context);
}

static void animate_grid(void *object, int32_t x) { lv_obj_set_x(object, x); }

static void grid_animation_done(lv_anim_t *animation)
{
    calendar_context_t *context = animation->user_data;
    context->animating = false;
    if(context->shift) {
        time_t day = context->app->calendar_day;
        select_day(context, context->app->calendar_folded ? app_calendar_add_days(day, context->shift * 7)
                                                         : app_calendar_add_months(day, context->shift));
    }
}

static void grid_touch(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev || context->app->route_pending || context->animating) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_PRESSED) {
        context->press = point;
        context->tracking = true;
        context->dragged = false;
        context->axis = 0;
    }
    else if(context->tracking) {
        int dx = point.x - context->press.x, dy = point.y - context->press.y;
        if(!context->axis && (abs(dx) > 8 || abs(dy) > 8)) context->axis = abs(dx) > abs(dy) ? 1 : 2;
        if(context->axis) context->dragged = true;
        if(code == LV_EVENT_PRESSING && context->axis == 1)
            lv_obj_set_x(context->track, -CAL_GRID_WIDTH + dx);
        if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            context->tracking = false;
            if(context->axis == 1) {
                context->shift = dx < -60 ? 1 : dx > 60 ? -1 : 0;
                context->animating = true;
                lv_anim_t animation;
                lv_anim_init(&animation);
                lv_anim_set_var(&animation, context->track);
                lv_anim_set_exec_cb(&animation, animate_grid);
                lv_anim_set_values(&animation, lv_obj_get_x(context->track), -CAL_GRID_WIDTH * (1 + context->shift));
                lv_anim_set_duration(&animation, 220);
                lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
                lv_anim_set_user_data(&animation, context);
                lv_anim_set_completed_cb(&animation, grid_animation_done);
                lv_anim_start(&animation);
            }
            else if(context->axis == 2 && abs(dy) > 40) {
                context->app->calendar_folded = dy < 0;
                ui_app_rebuild(context->app);
            }
        }
    }
}

static void timeline_touch(lv_event_t *event)
{
    calendar_context_t *context = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev || context->app->route_pending) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if(lv_event_get_code(event) == LV_EVENT_PRESSED) {
        context->press = point;
        context->tracking = true;
        context->dragged = false;
    }
    else if(context->tracking && (lv_event_get_code(event) == LV_EVENT_RELEASED ||
                                 lv_event_get_code(event) == LV_EVENT_PRESS_LOST)) {
        context->tracking = false;
        int dx = point.x - context->press.x, dy = point.y - context->press.y;
        context->dragged = abs(dx) > 10 || abs(dy) > 10;
        if(abs(dx) > 60 && abs(dx) > abs(dy) * 1.4)
            select_day(context, app_calendar_add_days(context->app->calendar_day, dx < 0 ? 1 : -1));
    }
}

static int month_grid(calendar_context_t *context)
{
    ui_app_t *app = context->app;
    struct tm selected;
    localtime_r(&app->calendar_day, &selected);
    time_t first = app_calendar_add_months(app->calendar_day, 0);
    struct tm tm;
    localtime_r(&first, &tm);
    int offset = (tm.tm_wday + 6) % 7;
    int month_days = (int)(difftime(app_calendar_add_months(first, 1), first) / 86400);
    int rows = app->calendar_folded ? 1 : (offset + month_days + 6) / 7;
    int height = 86 + rows * CAL_DAY_HEIGHT;
    lv_obj_t *card = calendar_card(context->root, 22, 96, 676, height, 28, false);
    calendar_day_binding_t *previous = &context->days[126], *next = &context->days[127];
    *previous = (calendar_day_binding_t){context, app->calendar_folded ? app_calendar_add_days(app->calendar_day, -7) : app_calendar_add_months(first, -1)};
    *next = (calendar_day_binding_t){context, app->calendar_folded ? app_calendar_add_days(app->calendar_day, 7) : app_calendar_add_months(first, 1)};
    lv_obj_t *back = calendar_button(card, 16, 8, 44, 44, 0, 22);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    calendar_icon(back, "back", 20, 0x525252, 12, 12);
    lv_obj_add_event_cb(back, period_clicked, LV_EVENT_CLICKED, previous);
    char text[64];
    snprintf(text, sizeof(text), "%d年%d月", selected.tm_year + 1900, selected.tm_mon + 1);
    lv_obj_t *month = calendar_label(card, text, 19, UI_FONT_SEMIBOLD, UI_REF_INK, 64, 14, 28);
    lv_obj_update_layout(month);
    lv_obj_t *forward = calendar_button(card, 68 + lv_obj_get_width(month), 8, 44, 44, 0, 22);
    lv_obj_set_style_bg_opa(forward, LV_OPA_TRANSP, 0);
    calendar_icon(forward, "fwd", 20, 0x525252, 12, 12);
    lv_obj_add_event_cb(forward, period_clicked, LV_EVENT_CLICKED, next);
    date_text(text, sizeof(text), app->calendar_day, false);
    lv_obj_t *date = calendar_label(card, text, 15, UI_FONT_REGULAR, UI_REF_FAINT, 368, 18, 22);
    lv_obj_set_width(date, 192);
    lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *fold = calendar_button(card, 568, 10, 92, 40, 0xe7e7e7, 20);
    calendar_label(fold, app->calendar_folded ? "月视图" : "周视图", 15, UI_FONT_MEDIUM, 0x525252, 14, 9, 22);
    lv_obj_t *arrow = calendar_icon(fold, "fwd", 15, 0x525252, 63, 12);
    lv_image_set_rotation(lv_obj_get_child(arrow, 0), app->calendar_folded ? 900 : 2700);
    lv_obj_add_event_cb(fold, fold_clicked, LV_EVENT_CLICKED, context);
    static const char *const names[] = {"一", "二", "三", "四", "五", "六", "日"};
    for(unsigned i = 0; i < 7; ++i) {
        lv_obj_t *name = calendar_label(card, names[i], 14, UI_FONT_REGULAR, UI_REF_FAINT,
                                         10 + i * CAL_GRID_WIDTH / 7, 52, 28);
        lv_obj_set_width(name, CAL_GRID_WIDTH / 7);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *viewport = ui_box(card, 10, 80, CAL_GRID_WIDTH, rows * CAL_DAY_HEIGHT, 0, 0);
    lv_obj_set_style_bg_opa(viewport, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(viewport, LV_OBJ_FLAG_CLICKABLE);
    context->track = ui_box(viewport, -CAL_GRID_WIDTH, 0, CAL_GRID_WIDTH * 3, 6 * CAL_DAY_HEIGHT, 0, 0);
    lv_obj_set_style_bg_opa(context->track, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->track, LV_OBJ_FLAG_CLICKABLE);
    for(int panel = -1; panel <= 1; ++panel) {
        time_t grid_month = app_calendar_add_months(first, panel), start;
        struct tm panel_tm;
        localtime_r(&grid_month, &panel_tm);
        int count;
        if(app->calendar_folded) {
            start = app_calendar_add_days(app->calendar_day, -(selected.tm_wday + 6) % 7 + panel * 7);
            count = 7;
        }
        else {
            int off = (panel_tm.tm_wday + 6) % 7;
            start = app_calendar_add_days(grid_month, -off);
            int days = (int)(difftime(app_calendar_add_months(grid_month, 1), grid_month) / 86400);
            count = (off + days + 6) / 7 * 7;
        }
        for(int i = 0; i < count; ++i) {
            time_t day = app_calendar_add_days(start, i);
            struct tm day_tm;
            localtime_r(&day, &day_tm);
            bool active = day == app->calendar_day;
            bool today = day == app_calendar_day(time(NULL));
            bool outside = !app->calendar_folded && day_tm.tm_mon != panel_tm.tm_mon;
            int x = (panel + 1) * CAL_GRID_WIDTH + i % 7 * CAL_GRID_WIDTH / 7;
            int width = (i % 7 + 1) * CAL_GRID_WIDTH / 7 - i % 7 * CAL_GRID_WIDTH / 7;
            lv_obj_t *button = calendar_button(context->track, x, i / 7 * CAL_DAY_HEIGHT, width, CAL_DAY_HEIGHT, 0, 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
            size_t index = (panel + 1) * 42 + i;
            context->days[index] = (calendar_day_binding_t){context, day};
            lv_obj_add_event_cb(button, day_clicked, LV_EVENT_CLICKED, &context->days[index]);
            lv_obj_t *circle = ui_box(button, (width - 36) / 2, 5, 36, 36, UI_REF_INK, 18);
            if(!active) lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
            if(today && !active) {
                lv_obj_set_style_border_color(circle, lv_color_hex(UI_REF_CLAY), 0);
                lv_obj_set_style_border_width(circle, 1, 0);
                lv_obj_set_style_border_opa(circle, 153, 0);
            }
            snprintf(text, sizeof(text), "%d", day_tm.tm_mday);
            ui_centered_label(circle, text, ui_ref_font(18, active || today ? UI_FONT_SEMIBOLD : UI_FONT_REGULAR),
                               active ? 0xffffff : today ? UI_REF_CLAY : outside ? 0xd4d4d4 : UI_REF_INK);
            app_calendar_segment_t segments[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
            size_t events = app->calendar.received ? app_calendar_segments(&app->calendar, day, segments) : 0;
            if(events > 3) events = 3;
            for(size_t j = 0; j < events; ++j) {
                const app_calendar_event_t *item = &app->calendar.events[segments[j].index];
                lv_obj_t *dot = ui_box(button, (width - (int)events * 8 + 3) / 2 + j * 8, 39, 5, 5,
                                         item->all_day || app_calendar_span(item) > 1 ? UI_REF_CLAY : UI_REF_FAINT, 3);
                if(outside) lv_obj_set_style_bg_opa(dot, 102, 0);
            }
        }
    }
    const lv_event_code_t codes[] = {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST};
    for(unsigned i = 0; i < 4; ++i) lv_obj_add_event_cb(viewport, grid_touch, codes[i], context);
    return height;
}

static void attach_event(calendar_context_t *context, lv_obj_t *button, const app_calendar_event_t *event, size_t index)
{
    context->events[index].context = context;
    snprintf(context->events[index].id, sizeof(context->events[index].id), "%s", event->data.event_id);
    lv_obj_add_event_cb(button, event_clicked, LV_EVENT_CLICKED, &context->events[index]);
}

static void empty_message(lv_obj_t *parent, const char *title, const char *body, bool loading)
{
    lv_obj_t *message = ui_box(parent, 25, 0, 626, 122, 0xffffff, 24);
    lv_obj_set_style_bg_opa(message, 230, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
    if(loading) calendar_icon(message, "sync", 19, UI_REF_MUTED, 36, 24);
    lv_obj_t *label = calendar_label(message, title, 20, UI_FONT_SEMIBOLD, UI_REF_INK, 20, 24, 30);
    lv_obj_set_width(label, 586);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    label = calendar_label(message, body, 15, UI_FONT_REGULAR, UI_REF_MUTED, 20, 63, 24);
    lv_obj_set_width(label, 586);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void timeline(calendar_context_t *context, int y)
{
    ui_app_t *app = context->app;
    app_calendar_segment_t segments[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
    size_t count = app->calendar.received ? app_calendar_segments(&app->calendar, app->calendar_day, segments) : 0;
    lv_obj_t *panel = calendar_card(context->root, 22, y, 676, 698 - y, 28, false);
    if(!app->calendar.received) {
        bool offline = app->calendar.connection_known && !app->calendar.connected;
        empty_message(panel, offline ? "暂时无法获取日程" : "正在从飞书日历同步…",
                       offline ? "连接恢复后会自动同步" : "", !offline);
        return;
    }
    int band_height = 0;
    for(size_t i = 0; i < count; ++i) if(segments[i].band) band_height = 65;
    if(band_height) {
        lv_obj_t *band = ui_box(panel, 0, 0, 676, 65, 0, 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(band, 1, 0);
        lv_obj_set_style_border_side(band, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(band, lv_color_hex(0xf5f5f5), 0);
        calendar_label(band, "全天", 14, UI_FONT_REGULAR, UI_REF_FAINT, 25, 20, 22);
        lv_obj_t *items = ui_box(band, 68, 0, 608, 64, 0, 0);
        lv_obj_set_style_bg_opa(items, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(items, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(items, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(items, LV_SCROLLBAR_MODE_OFF);
        int x = 0;
        for(size_t i = 0; i < count; ++i) {
            if(!segments[i].band) continue;
            const app_calendar_event_t *event = &app->calendar.events[segments[i].index];
            bool live = event->start <= time(NULL) && event->end > time(NULL);
            int width = 180;
            lv_point_t bounds;
            lv_text_get_size(&bounds, event->data.summary, ui_ref_font(15, UI_FONT_MEDIUM), 0, 0, 420, LV_TEXT_FLAG_NONE);
            width = bounds.x + 28 + (app_calendar_span(event) > 1 ? 65 : 0) + (live ? 16 : 0);
            if(width > 420) width = 420;
            lv_obj_t *button = calendar_button(items, x, 10, width, 44, live ? 0xfcece5 : 0xe7e7e7, 14);
            attach_event(context, button, event, i);
            int tx = live ? 30 : 14;
            if(live) ui_box(button, 14, 18, 8, 8, UI_REF_CLAY, 4);
            lv_obj_t *title = calendar_label(button, event->data.summary, 15, UI_FONT_MEDIUM, UI_REF_INK, tx, 11, 22);
            lv_obj_set_size(title, width - tx - 14 - (app_calendar_span(event) > 1 ? 65 : 0), 22);
            lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
            if(app_calendar_span(event) > 1) {
                char text[48];
                snprintf(text, sizeof(text), "%u/%u 天", app_calendar_day_number(event, app->calendar_day), app_calendar_span(event));
                calendar_label(button, text, 13, UI_FONT_REGULAR, UI_REF_MUTED, width - 64, 12, 20);
            }
            x += width + 8;
        }
    }
    context->scroller = ui_box(panel, 0, band_height, 676, 698 - y - band_height, 0, 0);
    lv_obj_set_style_bg_opa(context->scroller, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->scroller, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(context->scroller, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *canvas = ui_box(context->scroller, 0, 0, 676, 24 * CAL_HOUR + 16, 0, 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    for(unsigned hour = 0; hour <= 24; ++hour) {
        int top = hour * CAL_HOUR + 8;
        char text[12];
        snprintf(text, sizeof(text), "%02u:00", hour);
        if(hour < 24) {
            lv_obj_t *label = calendar_label(canvas, text, 14, UI_FONT_REGULAR, UI_REF_FAINT, 0, top - 11, 22);
            lv_obj_set_width(label, 48);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
        ui_box(canvas, 60, top, 604, 1, 0xf5f5f5, 0);
    }
    int initial_minute = 480;
    bool first = true;
    for(size_t i = 0; i < count; ++i) {
        const app_calendar_segment_t *segment = &segments[i];
        if(segment->band) continue;
        const app_calendar_event_t *event = &app->calendar.events[segment->index];
        if(first) initial_minute = segment->start_minute - 30, first = false;
        int height = (segment->end_minute - segment->start_minute) * CAL_HOUR / 60 - 3;
        if(height < 38) height = 38;
        int x = 64 + segment->column * 600 / segment->columns + (segment->column ? 3 : 0);
        int width = 600 / segment->columns - (segment->columns > 1 ? 3 : 0);
        bool done = event->end <= time(NULL), live = event->start <= time(NULL) && !done;
        uint32_t ink = done ? UI_REF_MUTED : UI_REF_INK;
        lv_obj_t *button = calendar_button(canvas, x, 9 + segment->start_minute * CAL_HOUR / 60,
                                            width, height, live ? 0xfbe3d8 : done ? 0xf1f0ee : 0xecebe8, 16);
        lv_obj_set_style_border_width(button, 5, 0);
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(live ? UI_REF_CLAY : done ? 0xd4d4d4 : UI_REF_INK), 0);
        lv_obj_set_style_pad_left(button, 0, 0);
        attach_event(context, button, event, i);
        int lines = height >= 92 ? 3 : height >= 64 ? 2 : 1;
        int available = width - 29 - (live && width >= 110 ? 64 : 0);
        lv_obj_t *title = calendar_label(button, event->data.summary, 15, UI_FONT_SEMIBOLD, ink, 12, 8, 21);
        lv_obj_set_width(title, available > 20 ? available : 20);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const lv_font_t *title_font = ui_ref_font(15, UI_FONT_SEMIBOLD);
        int line_space = title_font->line_height < 21 ? 21 - title_font->line_height : 0;
        lv_obj_set_style_text_line_space(title, line_space, 0);
        int title_height = title_font->line_height;
        if(lines > 1 && height >= 86) title_height = title_height * 2 + line_space;
        lv_obj_set_height(title, title_height);
        if(live && width >= 110) {
            lv_obj_t *badge = ui_box(button, width - 67, 8, 55, 24, UI_REF_CLAY, 12);
            ui_centered_label(badge, "进行中", ui_ref_font(13, UI_FONT_MEDIUM), 0xffffff);
        }
        if(lines > 1) {
            char start[32], end[32], text[96];
            hours_text(start, sizeof(start), event->start);
            hours_text(end, sizeof(end), event->end);
            snprintf(text, sizeof(text), "%s – %s%s", start, end, app_calendar_span(event) > 1 ? " · 跨天" : "");
            lv_obj_update_layout(title);
            title_height = lv_obj_get_height(title);
            lv_point_t bounds;
            lv_text_get_size(&bounds, event->data.summary, title_font, 0, line_space, available > 20 ? available : 20, LV_TEXT_FLAG_NONE);
            if(bounds.y < title_height) title_height = bounds.y;
            int detail_top = 10 + title_height;
            lv_obj_t *detail = calendar_label(button, text, 14, UI_FONT_REGULAR, UI_REF_MUTED, 12, detail_top, 21);
            lv_obj_set_size(detail, width - 29, 21);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            if(lines > 2 && detail_top + 44 <= height && event->data.location[0]) {
                calendar_icon(button, "pin", 14, UI_REF_MUTED, 12, detail_top + 26);
                detail = calendar_label(button, event->data.location, 14, UI_FONT_REGULAR, UI_REF_MUTED, 30, detail_top + 23, 21);
                lv_obj_set_size(detail, width - 47, 21);
                lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            }
        }
    }
    time_t now = time(NULL);
    if(app->calendar_day == app_calendar_day(now)) {
        struct tm tm;
        localtime_r(&now, &tm);
        int minute = tm.tm_hour * 60 + tm.tm_min;
        initial_minute = minute - 50;
        context->now_line = ui_box(canvas, 56, 3 + minute * CAL_HOUR / 60, 608, 10, 0, 0);
        lv_obj_set_style_bg_opa(context->now_line, LV_OPA_TRANSP, 0);
        ui_box(context->now_line, 8, 4, 600, 2, UI_REF_CLAY, 0);
        lv_obj_t *dot = ui_box(context->now_line, 0, 0, 10, 10, UI_REF_CLAY, 5);
        lv_obj_set_style_outline_width(dot, 2, 0);
        lv_obj_set_style_outline_color(dot, lv_color_hex(0xffffff), 0);
    }
    lv_obj_update_layout(context->scroller);
    int scroll = app->calendar_scroll_valid ? app->calendar_scroll_y : initial_minute * CAL_HOUR / 60;
    lv_obj_scroll_to_y(context->scroller, scroll > 0 ? scroll : 0, LV_ANIM_OFF);
    const lv_event_code_t codes[] = {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST};
    for(unsigned i = 0; i < 3; ++i) lv_obj_add_event_cb(context->scroller, timeline_touch, codes[i], context);
    if(!count) empty_message(panel, "这一天没有安排", app->calendar.connected ? "日程已同步" : "显示上次同步的数据", false);
}

static int event_phase(const app_calendar_event_t *event, time_t now)
{
    return now < event->start ? 0 : now < event->end ? 1 : 2;
}

static void calendar_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    (void)model;
    calendar_context_t *context = user_data;
    if(context->app->route_pending || context->animating || context->tracking) return;
    if(changes != APP_CHANGE_ALL && !(changes & (APP_CHANGE_CLOCK | APP_CHANGE_CALENDAR))) return;
    time_t now = time(NULL);
    bool refresh = context->generation != context->app->calendar.generation;
    if(context->minute != now / 60) {
        refresh |= context->detail || app_calendar_day(now) != app_calendar_day(context->minute * 60);
        for(size_t i = 0; !refresh && i < context->app->calendar.count; ++i) {
            const app_calendar_event_t *event = &context->app->calendar.events[i];
            refresh = event_phase(event, context->minute * 60) != event_phase(event, now);
        }
        context->minute = now / 60;
        if(!refresh && context->now_line) {
            struct tm tm;
            localtime_r(&now, &tm);
            lv_obj_set_y(context->now_line, 3 + (tm.tm_hour * 60 + tm.tm_min) * CAL_HOUR / 60);
        }
    }
    if(refresh) {
        if(context->scroller) {
            if(context->detail) context->app->calendar_detail_scroll_y = lv_obj_get_scroll_y(context->scroller);
            else {
                context->app->calendar_scroll_y = lv_obj_get_scroll_y(context->scroller);
                context->app->calendar_scroll_valid = true;
            }
        }
        ui_app_rebuild(context->app);
    }
}

static void calendar_destroy(void *user_data)
{
    calendar_context_t *context = user_data;
    if(context->track) lv_anim_delete(context->track, animate_grid);
    free(context);
}

static calendar_context_t *calendar_create(ui_app_t *app, bool detail)
{
    calendar_context_t *context = calloc(1, sizeof(*context));
    if(!context) return NULL;
    context->app = app;
    context->detail = detail;
    context->generation = app->calendar.generation;
    context->minute = time(NULL) / 60;
    context->root = ui_ref_page(app);
    if(!app->calendar_day) app->calendar_day = app_calendar_day(time(NULL));
    header(context);
    return context;
}

ui_screen_t ui_calendar_screen_create(ui_app_t *app)
{
    calendar_context_t *context = calendar_create(app, false);
    if(!context) return (ui_screen_t){0};
    int y = 96 + month_grid(context) + 10;
    if(app->calendar.connection_known && !app->calendar.connected && app->calendar.received) {
        lv_obj_t *banner = ui_box(context->root, 22, y, 676, 40, 0xe4e1dc, 20);
        calendar_icon(banner, "cloudOff", 18, 0x525252, 16, 11);
        char text[128], when[64];
        app_calendar_when(when, sizeof(when), app->calendar.received_at, time(NULL));
        snprintf(text, sizeof(text), "离线中，显示上次同步的数据 · %s", when);
        lv_obj_t *label = calendar_label(banner, text, 15, UI_FONT_REGULAR, 0x525252, 42, 9, 22);
        lv_obj_set_width(label, 618);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        y += 50;
    }
    timeline(context, y);
    return (ui_screen_t){.root = context->root, .context = context,
                         .update = calendar_update, .destroy = calendar_destroy};
}

static int detail_row(lv_obj_t *parent, int y, const char *icon, const char *text, const char *sub, bool border)
{
    if(border) ui_box(parent, 22, y, 632, 1, 0xf5f5f5, 0);
    lv_obj_t *circle = calendar_card(parent, 22, y + 16, 44, 44, 22, true);
    calendar_icon(circle, icon, 21, UI_REF_MUTED, 11, 11);
    lv_obj_t *label = calendar_label(parent, text, 18, UI_FONT_MEDIUM, UI_REF_INK, 82, y + 24, 25);
    lv_obj_set_width(label, 572);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(label);
    int bottom = y + 24 + lv_obj_get_height(label);
    if(sub && sub[0]) {
        label = calendar_label(parent, sub, 15, UI_FONT_REGULAR, UI_REF_FAINT, 82, bottom + 2, 22);
        lv_obj_set_width(label, 572);
        lv_obj_update_layout(label);
        bottom = lv_obj_get_y(label) + lv_obj_get_height(label);
    }
    int height = bottom - y + 16;
    return height < 76 ? 76 : height;
}

ui_screen_t ui_calendar_detail_screen_create(ui_app_t *app)
{
    calendar_context_t *context = calendar_create(app, true);
    if(!context) return (ui_screen_t){0};
    const app_calendar_event_t *event = app_calendar_find(&app->calendar, app->calendar_selected);
    if(!event) {
        lv_obj_t *text = calendar_label(context->root, "这条日程已不存在", 17, UI_FONT_REGULAR, UI_REF_MUTED, 0, 350, 26);
        lv_obj_set_width(text, 720);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    }
    else {
        lv_obj_t *scroll = ui_box(context->root, 0, 96, 720, 602, 0, 0);
        lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
        context->scroller = scroll;
        lv_obj_t *summary = calendar_card(scroll, 22, 0, 676, 128, 30, false);
        time_t now = time(NULL);
        bool live = event->start <= now && event->end > now;
        char badge[80];
        badge[0] = 0;
        if(event->end <= now) snprintf(badge, sizeof(badge), "已结束");
        else if(live) snprintf(badge, sizeof(badge), "进行中");
        else if(!event->all_day && app_calendar_day(event->start) == app_calendar_day(now))
            app_calendar_countdown(badge, sizeof(badge), event->start, now);
        int title_y = 24;
        if(badge[0] || event->all_day || app_calendar_span(event) > 1) {
            int x = 24;
            const char *badges[] = {badge, event->all_day ? "全天日程" : "", app_calendar_span(event) > 1 ? "跨天" : ""};
            for(unsigned i = 0; i < 3; ++i) {
                if(!badges[i][0]) continue;
                lv_point_t bounds;
                lv_text_get_size(&bounds, badges[i], ui_ref_font(15, UI_FONT_MEDIUM), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                lv_obj_t *pill = ui_box(summary, x, 24, bounds.x + 24, 30, i == 0 && live ? UI_REF_CLAY : 0xe7e7e7, 15);
                ui_centered_label(pill, badges[i], ui_ref_font(15, UI_FONT_MEDIUM), i == 0 && live ? 0xffffff : 0x525252);
                x += bounds.x + 32;
            }
            title_y += 42;
        }
        lv_obj_t *title = calendar_label(summary, event->data.summary[0] ? event->data.summary : "未命名日程", 28, UI_FONT_SEMIBOLD, UI_REF_INK, 24, title_y, 39);
        lv_obj_set_width(title, 628);
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(title);
        int summary_height = lv_obj_get_y(title) + lv_obj_get_height(title) + 24;
        lv_obj_set_height(summary, summary_height);
        lv_obj_t *details = calendar_card(scroll, 22, summary_height + 12, 676, 200, 26, false);
        char date[160], end_date[64], start_time[32], end_time[32], text[128], sub[64];
        unsigned span = app_calendar_span(event);
        date_text(date, sizeof(date), event->start, span == 1);
        if(span > 1) {
            date_text(end_date, sizeof(end_date), event->all_day ? event->end - 1 : event->end, false);
            size_t length = strlen(date);
            snprintf(date + length, sizeof(date) - length, " – %s", end_date);
            snprintf(sub, sizeof(sub), "共 %u 天", span);
        }
        else sub[0] = 0;
        int row_y = 6;
        row_y += detail_row(details, row_y, "cal", date, sub, false);
        hours_text(start_time, sizeof(start_time), event->start);
        hours_text(end_time, sizeof(end_time), event->end);
        if(event->all_day) snprintf(text, sizeof(text), "全天");
        else if(span > 1) {
            struct tm start, end;
            localtime_r(&event->start, &start);
            localtime_r(&event->end, &end);
            snprintf(text, sizeof(text), "%d/%d %s – %d/%d %s", start.tm_mon + 1, start.tm_mday, start_time,
                     end.tm_mon + 1, end.tm_mday, end_time);
        }
        else snprintf(text, sizeof(text), "%s – %s", start_time, end_time);
        int minutes = (int)(difftime(event->end, event->start) / 60);
        sub[0] = 0;
        if(!event->all_day) {
            if(minutes < 60) snprintf(sub, sizeof(sub), "%d 分钟", minutes);
            else if(minutes % 60) snprintf(sub, sizeof(sub), "%d 小时 %d 分钟", minutes / 60, minutes % 60);
            else snprintf(sub, sizeof(sub), "%d 小时", minutes / 60);
        }
        row_y += detail_row(details, row_y, "clock", text, sub, true);
        if(event->data.location[0]) row_y += detail_row(details, row_y, "pin", event->data.location, NULL, true);
        lv_obj_set_height(details, row_y + 6);
        if(!app->calendar.connected) {
            lv_obj_t *offline = calendar_label(scroll, "当前为离线数据", 15, UI_FONT_REGULAR, UI_REF_FAINT,
                                                22, summary_height + row_y + 36, 24);
            lv_obj_set_width(offline, 676);
            lv_obj_set_style_text_align(offline, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_update_layout(scroll);
        lv_obj_scroll_to_y(scroll, app->calendar_detail_scroll_y, LV_ANIM_OFF);
    }
    return (ui_screen_t){.root = context->root, .context = context,
                         .update = calendar_update, .destroy = calendar_destroy};
}
