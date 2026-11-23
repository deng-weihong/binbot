#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "binbot/platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static time_t frozen_now = 1791993540; /* 2026-10-14 15:59 UTC. */

time_t __wrap_time(time_t *result)
{
    if(result) *result = frozen_now;
    return frozen_now;
}

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void settle(ui_app_t *app)
{
    lv_timer_ready(app->clock_timer);
    lv_tick_inc(1000);
    app_state_tick(app->state, lv_tick_get());
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void check_clock(ui_app_t *app, time_t timestamp, const char *clock,
                         const char *date, const char *home_date, const char *greeting)
{
    frozen_now = timestamp;
    char text[64];
    ui_format_time(text, sizeof(text));
    assert(!strcmp(text, clock));
    ui_format_date(text, sizeof(text));
    assert(!strcmp(text, date));
    struct tm local;
    assert(localtime_r(&frozen_now, &local));
    assert(local.tm_isdst == 0 && mktime(&local) == frozen_now);
    settle(app);
    assert(!strcmp(lv_label_get_text(app->status_time), clock));
    lv_obj_t *label = find_label(app->current.root, clock);
    assert(label && lv_obj_get_width(label) <= 195);
    lv_point_t bounds;
    lv_text_get_size(&bounds, clock, lv_obj_get_style_text_font(label, 0), 0, 0,
                      LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(bounds.x <= lv_obj_get_width(label));
    assert(find_label(app->current.root, home_date));
    label = find_label(app->current.root, greeting); assert(label);
    lv_text_get_size(&bounds, greeting, lv_obj_get_style_text_font(label, 0), 0, 0,
                      LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(bounds.x <= lv_obj_get_width(label));
}

static void check_alarm(ui_app_t *app, time_t timestamp, const char *countdown)
{
    frozen_now = timestamp;
    lv_display_trigger_activity(NULL);
    lv_tick_inc(30000);
    settle(app);
    assert(find_label(app->current.root, countdown));
}

int main(int argc, char **argv)
{
    if(argc > 1 && !strcmp(argv[1], "unset")) assert(!unsetenv("TZ"));
    tzset();
    char directory[] = "/tmp/binbot-timezone-ui-XXXXXX";
    assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    lv_init();
    assert(platform_initialize());
    assert(getenv("TZ") && !strcmp(getenv("TZ"), "CST-8"));
    lv_tick_set_cb(NULL);
    app_state_t *state = app_state_create(); assert(state);
    while(app_state_model(state)->alarm_count) assert(app_state_delete_alarm(state, 0));
    assert(app_state_save_alarm(state, 0, "00:01", "测试", 1U << 3, true));
    ui_app_t *app = ui_app_create(state); assert(app);
    check_clock(app, 1791993540, "23:59", "10月14日 · 周三", "10月14日 周三", "夜深了，早点休息");
    check_clock(app, 1791993600, "00:00", "10月15日 · 周四", "10月15日 周四", "夜深了，早点休息");
    check_clock(app, 1798732740, "23:59", "12月31日 · 周四", "12月31日 周四", "夜深了，早点休息");
    check_clock(app, 1798732800, "00:00", "1月1日 · 周五", "1月1日 周五", "夜深了，早点休息");
    check_clock(app, 1782878700, "12:05", "7月1日 · 周三", "7月1日 周三", "下午好，保持专注");
    check_clock(app, 1791993600 + 5 * 3600, "05:00", "10月15日 · 周四", "10月15日 周四", "早上好，开启新一天");
    check_clock(app, 1791993600 + 9 * 3600, "09:00", "10月15日 · 周四", "10月15日 周四", "上午好，保持专注");
    check_clock(app, 1791993600 + 12 * 3600, "12:00", "10月15日 · 周四", "10月15日 周四", "下午好，保持专注");
    check_clock(app, 1791993600 + 18 * 3600, "18:00", "10月15日 · 周四", "10月15日 周四", "晚上好，放松一下");
    check_clock(app, 1791993600 + 22 * 3600, "22:00", "10月15日 · 周四", "10月15日 周四", "夜深了，早点休息");

    frozen_now = 1791993540;
    ui_app_navigate(app, UI_ROUTE_ALARM);
    settle(app);
    assert(app->route == UI_ROUTE_ALARM);
    check_alarm(app, 1791993540, "0 小时 2 分钟后");
    check_alarm(app, 1791993600, "0 小时 1 分钟后");
    check_alarm(app, 1791993660, "168 小时 0 分钟后");
    frozen_now = 1798732740;
    assert(app_state_save_alarm(state, 0, "00:01", "测试", 1U << 4, true));
    check_alarm(app, 1798732740, "0 小时 2 分钟后");
    check_alarm(app, 1798732800, "0 小时 1 分钟后");
    frozen_now = 1782878700;
    assert(app_state_save_alarm(state, 0, "12:10", "测试", 1U << 2, true));
    check_alarm(app, 1782878700, "0 小时 5 分钟后");

    ui_app_destroy(app);
    app_state_destroy(state);
    platform_shutdown();
    lv_deinit();
    assert(!rmdir(directory));
    puts("China timezone, midnight, new year, summer clock and weekly alarms passed");
    return 0;
}
