#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ui_calendar_notice {
    ui_app_t *app;
    lv_obj_t *root;
    char event_id[64];
    time_t day;
    bool single;
};

static void hide_notice(ui_calendar_notice_t *notice)
{
    if(notice->root) lv_obj_delete(notice->root);
    notice->root = NULL;
}

static void closed(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    hide_notice(lv_event_get_user_data(event));
}

static void opened(lv_event_t *event)
{
    ui_calendar_notice_t *notice = lv_event_get_user_data(event);
    ui_app_t *app = notice->app;
    if(app->route_pending) return;
    lv_event_stop_bubbling(event);
    app->calendar_day = notice->day;
    app->calendar_scroll_valid = false;
    ui_app_close_control_center(app);
    if(notice->single && app_calendar_find(&app->calendar, notice->event_id))
        ui_calendar_open_event(app, notice->event_id, app->route == UI_ROUTE_HOME);
    else ui_app_navigate(app, UI_ROUTE_CALENDAR);
    hide_notice(notice);
}

static lv_obj_t *notice_label(lv_obj_t *root, const char *text, unsigned size,
                              ui_font_weight_t weight, uint32_t color, int x, int y, int width)
{
    lv_obj_t *label = ui_ref_label(root, text, size, weight, color, x, y);
    lv_obj_set_size(label, width, size + 10);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

ui_calendar_notice_t *ui_calendar_notice_create(ui_app_t *app)
{
    ui_calendar_notice_t *notice = calloc(1, sizeof(*notice));
    if(notice) notice->app = app;
    return notice;
}

void ui_calendar_notice_raise(ui_calendar_notice_t *notice)
{
    if(!notice) return;
    if(notice->root) lv_obj_move_foreground(notice->root);
    ui_gesture_popup_raise(notice->app->gestures);
}

void ui_calendar_notice_show(ui_calendar_notice_t *notice, const app_calendar_changes_t *changes)
{
    if(!notice || !changes) return;
    unsigned total = changes->added + changes->updated + changes->removed;
    if(!total) return;
    hide_notice(notice);
    ui_screensaver_wake(notice->app->screensaver);
    bool vibrating = ui_app_vibrate(notice->app, 100);
    const app_calendar_event_t *event = &changes->event;
    notice->single = total == 1 && event->data.event_id[0];
    notice->day = app_calendar_day(event->data.event_id[0] ? event->start : time(NULL));
    snprintf(notice->event_id, sizeof(notice->event_id), "%s", event->data.event_id);
    const char *heading = total > 1 ? "日程有更新" : changes->added ? "新增日程" :
                          changes->updated ? "日程已更新" : "日程已移除";
    char summary[160], when[128];
    if(notice->single) {
        snprintf(summary, sizeof(summary), "%s", event->data.summary);
        for(char *c = summary; *c; ++c) if(*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
        if(event->all_day) {
            struct tm date;
            localtime_r(&event->start, &date);
            snprintf(when, sizeof(when), "%d月%d日 · 全天", date.tm_mon + 1, date.tm_mday);
        } else {
            char start[64];
            app_calendar_when(start, sizeof(start), event->start, time(NULL));
            struct tm end;
            localtime_r(&event->end, &end);
            if(app_calendar_day(event->start) == app_calendar_day(event->end))
                snprintf(when, sizeof(when), "%s - %02d:%02d", start, end.tm_hour, end.tm_min);
            else {
                char finish[64];
                app_calendar_when(finish, sizeof(finish), event->end, time(NULL));
                snprintf(when, sizeof(when), "%.60s 至 %.60s", start, finish);
            }
        }
    } else if(total > 1) {
        snprintf(summary, sizeof(summary), "新增 %u 项 · 修改 %u 项 · 移除 %u 项",
                 changes->added, changes->updated, changes->removed);
        snprintf(when, sizeof(when), "飞书日历");
    } else {
        snprintf(summary, sizeof(summary), "飞书日历");
        snprintf(when, sizeof(when), "%u 项日程", total);
    }
    lv_obj_t *root = ui_button(notice->app->root, 90, 108, 540, 120, 0xffffff, 24);
    notice->root = root;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, opened, LV_EVENT_CLICKED, notice);
    ui_ref_shadow(root, 0, 42, 28, -8, 8);
    lv_obj_t *tile = ui_box(root, 16, 20, 46, 46, 0xe9f3ed, 15);
    lv_obj_t *icon = ui_ref_icon(tile, "cal", 24, 0x3d8062);
    lv_obj_center(icon);
    notice_label(root, heading, 15, UI_FONT_MEDIUM, UI_REF_MUTED, 78, 12, 400);
    notice_label(root, summary, 20, UI_FONT_SEMIBOLD, UI_REF_INK, 78, 38, 400);
    notice_label(root, when, 16, UI_FONT_REGULAR, UI_REF_MUTED, 78, 80, 440);
    lv_obj_t *close = ui_button(root, 490, 8, 40, 40, 0xffffff, 12);
    lv_obj_remove_flag(close, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(close, closed, LV_EVENT_CLICKED, notice);
    icon = ui_ref_icon(close, "x", 20, UI_REF_MUTED);
    lv_obj_center(icon);
    ui_calendar_notice_raise(notice);
    fprintf(stdout, "ui: calendar notice added=%u updated=%u removed=%u vibration=%s\n",
            changes->added, changes->updated, changes->removed, vibrating ? "queued" : "unavailable");
}

void ui_calendar_notice_destroy(ui_calendar_notice_t *notice)
{
    if(!notice) return;
    hide_notice(notice);
    free(notice);
}
