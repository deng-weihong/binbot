#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static time_t now;

_Static_assert(offsetof(binbot_audio_calendar_state_t, events) == 8, "Calendar IPC event offset changed");
_Static_assert(sizeof(binbot_audio_calendar_state_t) == 8 +
               BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY * sizeof(binbot_audio_calendar_event_t),
               "Calendar IPC packet size changed");

time_t __wrap_time(time_t *result)
{
    if(result) *result = now;
    return now;
}

static time_t at(int day, int hour)
{
    struct tm value = {.tm_year = 126, .tm_mon = 9, .tm_mday = day,
                        .tm_hour = hour, .tm_isdst = -1};
    return mktime(&value);
}

static void add_event(binbot_audio_calendar_state_t *state, const char *id, const char *summary,
                       int day, int hour)
{
    binbot_audio_calendar_event_t *event = &state->events[state->event_count++];
    snprintf(event->event_id, sizeof(event->event_id), "%s", id);
    snprintf(event->summary, sizeof(event->summary), "%s", summary);
    snprintf(event->start_timestamp, sizeof(event->start_timestamp), "%lld", (long long)at(day, hour));
    snprintf(event->end_timestamp, sizeof(event->end_timestamp), "%lld", (long long)at(day, hour + 1));
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *banner(ui_app_t *app, const char *heading)
{
    lv_obj_t *found = label(app->root, heading);
    assert(found);
    lv_obj_t *root = lv_obj_get_parent(found);
    assert(lv_obj_get_parent(root) == app->root);
    assert(lv_obj_get_x(root) == 90 && lv_obj_get_y(root) == 108);
    assert(lv_obj_get_width(root) == 540 && lv_obj_get_height(root) == 120);
    return root;
}

static void advance(uint32_t milliseconds)
{
    lv_tick_inc(milliseconds);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_CALENDAR_NOTICE_SNAPSHOT");
    if(!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
    assert(frame && frame->header.w == 720 && frame->header.h == 720);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *pixel = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {pixel[2], pixel[1], pixel[0]};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(!fclose(file));
    lv_draw_buf_destroy(frame);
}

static void bounded_snapshots(void)
{
    app_calendar_t calendar = {0};
    binbot_audio_calendar_state_t initial = {.revision = 100,
        .changes_added = BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT};
    for(unsigned i = 0; i < BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY; ++i) {
        char id[32]; snprintf(id, sizeof(id), "existing-%u", i);
        add_event(&initial, id, id, 19, 10 + i);
    }
    assert(app_calendar_receive(&calendar, &initial, now));
    assert(!calendar.changes.added && !calendar.changes.updated && !calendar.changes.removed);
    binbot_audio_calendar_state_t changed = {.revision = 101,
        .changes_added = BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT | 1};
    add_event(&changed, "new-visible", "New event", 19, 9);
    for(unsigned i = 0; i < BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY - 1; ++i)
        changed.events[changed.event_count++] = initial.events[i];
    assert(app_calendar_receive(&calendar, &changed, now));
    assert(calendar.changes.added == 1 && !calendar.changes.updated && !calendar.changes.removed);
    assert(!strcmp(calendar.changes.event.data.event_id, "new-visible"));
    assert(app_calendar_receive(&calendar, &changed, now));
    assert(!calendar.changes.added && !calendar.changes.updated && !calendar.changes.removed);
    initial.revision = 102; initial.changes_removed = 1;
    assert(app_calendar_receive(&calendar, &initial, now));
    assert(!calendar.changes.added && !calendar.changes.updated && calendar.changes.removed == 1);
    assert(!strcmp(calendar.changes.event.data.event_id, "new-visible"));
    initial.revision = 103; initial.changes_removed = 0; initial.changes_updated = 1;
    assert(app_calendar_receive(&calendar, &initial, now));
    assert(calendar.changes.updated == 1 && !calendar.changes.event.data.event_id[0]);
}

int main(void)
{
    char directory[] = "/tmp/binbot-calendar-notice-XXXXXX";
    assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    now = at(19, 9);
    bounded_snapshots();
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app && app->calendar_notice);
    advance(50);

    binbot_audio_calendar_state_t input = {.revision = 5};
    add_event(&input, "existing", "已有日程", 19, 10);
    assert(ui_app_handle_calendar_state(app, &input));
    assert(!app->calendar.changes.added && !label(app->root, "新增日程"));
    ++input.revision;
    add_event(&input, "new", "产品方案评审", 20, 14);
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    lv_obj_t *root = banner(app, "新增日程");
    assert(app->route == UI_ROUTE_HOME && app->calendar.changes.added == 1);
    assert(label(root, "产品方案评审") && label(root, "明天 14:00 - 15:00"));
    snapshot(app, "added");
    advance(1000);
    ++input.revision;
    binbot_audio_calendar_event_t swap = input.events[0];
    input.events[0] = input.events[1]; input.events[1] = swap;
    assert(ui_app_handle_calendar_state(app, &input));
    assert(!app->calendar.changes.added && !app->calendar.changes.updated && !app->calendar.changes.removed);
    assert(banner(app, "新增日程") == root);
    advance(5001);
    assert(banner(app, "新增日程") == root);
    lv_obj_send_event(lv_obj_get_child(root, -1), LV_EVENT_CLICKED, NULL); advance(50);
    assert(!label(app->root, "新增日程"));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_DISCONNECTED);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE);
    assert(ui_app_handle_calendar_state(app, &input));
    assert(!label(app->root, "日程已更新") && !label(app->root, "新增日程"));

    ui_screensaver_show(app->screensaver); advance(400);
    assert(ui_screensaver_active(app->screensaver));
    snprintf(input.events[0].summary, sizeof(input.events[0].summary), "评审调整\n改至下午");
    snprintf(input.events[0].start_timestamp, sizeof(input.events[0].start_timestamp), "%lld", (long long)at(20, 15));
    snprintf(input.events[0].end_timestamp, sizeof(input.events[0].end_timestamp), "%lld", (long long)at(20, 16));
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    assert(!ui_screensaver_active(app->screensaver));
    root = banner(app, "日程已更新");
    assert(app->calendar.changes.updated == 1 && label(root, "评审调整 改至下午"));
    assert(label(root, "明天 15:00 - 16:00"));
    snapshot(app, "updated");
    ui_app_open_control_center(app); advance(400);
    assert(lv_obj_get_child(app->root, -1) == root);
    lv_obj_send_event(root, LV_EVENT_CLICKED, NULL); advance(50);
    assert(app->route == UI_ROUTE_CALENDAR_DETAIL && !strcmp(app->calendar_selected, "new"));
    assert(!label(app->root, "日程已更新"));

    input.events[0] = input.events[1]; input.event_count = 1;
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    root = banner(app, "日程已移除");
    assert(app->calendar.changes.removed == 1 && label(root, "评审调整 改至下午"));
    snapshot(app, "removed");
    lv_obj_send_event(root, LV_EVENT_CLICKED, NULL); advance(50);
    assert(app->route == UI_ROUTE_CALENDAR && app->calendar_day == at(20, 0));

    add_event(&input, "all-day", "全天安排", 21, 9);
    binbot_audio_calendar_event_t *all = &input.events[1];
    all->start_timestamp[0] = all->end_timestamp[0] = 0;
    snprintf(all->start_date, sizeof(all->start_date), "2026-10-21");
    snprintf(all->end_date, sizeof(all->end_date), "2026-10-22");
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    root = banner(app, "新增日程");
    assert(label(root, "10月21日 · 全天"));
    snapshot(app, "all-day");
    lv_obj_send_event(lv_obj_get_child(root, -1), LV_EVENT_CLICKED, NULL); advance(50);
    assert(app->route == UI_ROUTE_CALENDAR && !label(app->root, "新增日程"));

    input.event_count = BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY + 1;
    assert(!ui_app_handle_calendar_state(app, &input));
    assert(app->calendar.count == 2 && !label(app->root, "日程有更新"));
    input.event_count = 2;
    memset(input.events[0].summary, 'x', sizeof(input.events[0].summary));
    assert(!ui_app_handle_calendar_state(app, &input));
    snprintf(input.events[0].summary, sizeof(input.events[0].summary), "已有日程");
    snprintf(input.events[0].status, sizeof(input.events[0].status), "cancelled");
    snprintf(input.events[1].location, sizeof(input.events[1].location), "新地点");
    add_event(&input, "batch", "另一项安排", 22, 10);
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    assert(app->calendar.changes.added == 1 && app->calendar.changes.updated == 1 && app->calendar.changes.removed == 1);
    root = banner(app, "日程有更新");
    assert(label(root, "新增 1 项 · 修改 1 项 · 移除 1 项"));
    snapshot(app, "batch");
    lv_obj_send_event(root, LV_EVENT_CLICKED, NULL); advance(50);
    assert(app->route == UI_ROUTE_CALENDAR && !label(app->root, "日程有更新"));

    const ui_route_t routes[] = {UI_ROUTE_HOME, UI_ROUTE_SETTINGS, UI_ROUTE_CALENDAR, UI_ROUTE_ABOUT};
    for(unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ui_app_navigate(app, routes[i]); advance(50);
        snprintf(input.events[1].location, sizeof(input.events[1].location), "地点 %u", i);
        assert(ui_app_handle_calendar_state(app, &input)); advance(50);
        root = banner(app, "日程已更新");
        assert(app->route == routes[i]);
        snprintf(input.events[1].location, sizeof(input.events[1].location), "再次修改 %u", i);
        assert(ui_app_handle_calendar_state(app, &input)); advance(50);
        assert(label(app->root, "日程已更新"));
        advance(6001);
        root = banner(app, "日程已更新");
        lv_obj_send_event(lv_obj_get_child(root, -1), LV_EVENT_CLICKED, NULL); advance(50);
        assert(!label(app->root, "日程已更新"));
    }
    ++input.revision;
    input.changes_added = BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT;
    input.changes_updated = 1;
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    root = banner(app, "日程已更新");
    assert(label(root, "飞书日历") && label(root, "1 项日程"));
    snapshot(app, "outside-cache");
    lv_obj_send_event(root, LV_EVENT_CLICKED, NULL); advance(50);
    assert(app->route == UI_ROUTE_CALENDAR && app->calendar_day == at(19, 0));
    ++input.revision;
    snprintf(input.events[1].location, sizeof(input.events[1].location), "关闭前更新");
    assert(ui_app_handle_calendar_state(app, &input)); advance(50);
    ui_app_destroy(app); app_state_destroy(state);
    advance(7000);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    assert(!rmdir(directory));
    puts("Calendar notices: baseline, replay, add, edit, remove, batch, navigation, acknowledgement and cleanup passed");
    return 0;
}
