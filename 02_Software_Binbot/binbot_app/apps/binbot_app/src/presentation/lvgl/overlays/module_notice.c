#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned profile; bool inserted; } module_notice_event_t;

struct ui_module_notice {
    ui_app_t *app;
    lv_obj_t *root;
    lv_timer_t *timer;
    uint32_t shown_at;
    module_notice_event_t pending[4];
    unsigned pending_count;
};

static void animate_y(void *object, int32_t value) { lv_obj_set_y(object, value); }
static void animate_opacity(void *object, int32_t value) { lv_obj_set_style_opa(object, value, 0); }

static void show_notice(ui_module_notice_t *notice, module_notice_event_t event);

static void advance_notice(lv_timer_t *timer)
{
    ui_module_notice_t *notice = lv_timer_get_user_data(timer);
    lv_obj_delete(notice->root);
    notice->root = NULL;
    if(notice->pending_count) {
        module_notice_event_t event = notice->pending[0];
        --notice->pending_count;
        memmove(notice->pending, notice->pending + 1, notice->pending_count * sizeof(event));
        show_notice(notice, event);
    } else {
        notice->timer = NULL;
        lv_timer_delete(timer);
    }
}

static void show_notice(ui_module_notice_t *notice, module_notice_event_t event)
{
    bool vibrating = ui_app_vibrate(notice->app, 50);
    fprintf(stdout, "ui: component %s profile=%u vibration=%s\n",
            event.inserted ? "inserted" : "removed", event.profile, vibrating ? "queued" : "unavailable");
    const char *name = "未知组件", *icon = "cube";
    switch(event.profile) {
        case 1: name = "笔筒"; icon = "pen"; break;
        case 2: name = "全息屏"; icon = "holo"; break;
        case 3: name = "转盘"; icon = "dial"; break;
        case 5: name = "氛围灯"; icon = "bulb"; break;
    }
    lv_obj_t *root = ui_box(notice->app->root, 150, 22, 420, 74, 0xffffff, 25);
    notice->root = root;
    ui_screensaver_raise(notice->app->screensaver);
    ui_gesture_popup_raise(notice->app->gestures);
    ui_ref_surface(root, "module_notice");
    lv_obj_t *tile = ui_box(root, 14, 13, 48, 48, event.inserted ? UI_REF_CLAY : 0xececec, 18);
    if(event.inserted) ui_ref_shadow(tile, UI_REF_CLAY, 255, 20, -6, 8);
    else ui_ref_surface(tile, "module_notice_icon");
    lv_obj_t *image = ui_ref_icon(tile, icon, 24, event.inserted ? 0xffffff : UI_REF_FAINT);
    lv_obj_center(image);
    ui_ref_label(root, event.inserted ? "组件已插入" : "组件已拔出",
                 15, UI_FONT_REGULAR, UI_REF_FAINT, 76, 13);
    char text[64];
    snprintf(text, sizeof(text), event.inserted ? "已识别：%s" : "%s已断开", name);
    lv_obj_t *label = ui_ref_label(root, text, 18, UI_FONT_SEMIBOLD, UI_REF_INK, 76, 38);
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    if(event.inserted) {
        lv_obj_t *halo = ui_box(root, 387, 29, 16, 16, 0x7ed38f, 8);
        lv_obj_set_style_bg_opa(halo, 64, 0);
    }
    ui_box(root, 390, 32, 10, 10, event.inserted ? 0x7ed38f : 0xd4d4d4, 5);
    notice->shown_at = lv_tick_get();
    uint32_t duration = notice->pending_count ? 900 : 2600;
    if(!notice->timer) notice->timer = lv_timer_create(advance_notice, duration, notice);
    else {
        lv_timer_set_period(notice->timer, duration);
        lv_timer_reset(notice->timer);
    }
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, root);
    lv_anim_set_duration(&animation, 350);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, animate_y);
    lv_anim_set_values(&animation, 34, 22);
    lv_anim_start(&animation);
    lv_anim_set_exec_cb(&animation, animate_opacity);
    lv_anim_set_values(&animation, 0, 255);
    lv_anim_start(&animation);
}

static void connection_event(unsigned profile, bool inserted, void *user_data)
{
    ui_module_notice_t *notice = user_data;
    module_notice_event_t event = {profile, inserted};
    if(!notice->root) {
        show_notice(notice, event);
        return;
    }
    unsigned capacity = sizeof(notice->pending) / sizeof(notice->pending[0]);
    if(notice->pending_count == capacity) {
        --notice->pending_count;
        memmove(notice->pending, notice->pending + 1, notice->pending_count * sizeof(event));
    }
    notice->pending[notice->pending_count++] = event;
    /* Keep replacements readable before showing the next connection event. */
    uint32_t elapsed = lv_tick_elaps(notice->shown_at);
    lv_timer_set_period(notice->timer, elapsed < 900 ? 900 - elapsed : 1);
    lv_timer_reset(notice->timer);
}

ui_module_notice_t *ui_module_notice_create(ui_app_t *app)
{
    ui_module_notice_t *notice = calloc(1, sizeof(*notice));
    if(!notice) return NULL;
    notice->app = app;
    app_modules_set_connection_observer(app->modules, connection_event, notice);
    return notice;
}

void ui_module_notice_raise(ui_module_notice_t *notice)
{
    if(notice && notice->root) lv_obj_move_foreground(notice->root);
    if(notice) ui_gesture_popup_raise(notice->app->gestures);
}

void ui_module_notice_destroy(ui_module_notice_t *notice)
{
    if(!notice) return;
    app_modules_set_connection_observer(notice->app->modules, NULL, NULL);
    if(notice->timer) lv_timer_delete(notice->timer);
    if(notice->root) lv_obj_delete(notice->root);
    free(notice);
}
