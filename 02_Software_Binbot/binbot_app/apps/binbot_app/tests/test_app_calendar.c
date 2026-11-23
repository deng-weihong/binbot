#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static time_t current_time;
static lv_indev_data_t pointer;
static lv_indev_t *pointer_input;
static void tap(lv_indev_t *input, int x, int y);

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = pointer;
}
time_t __wrap_time(time_t *result)
{
    if(result) *result = current_time;
    return current_time;
}

static time_t at(int month, int day, int hour, int minute)
{
    struct tm value = {.tm_year = 126, .tm_mon = month - 1, .tm_mday = day,
                        .tm_hour = hour, .tm_min = minute, .tm_isdst = -1};
    return mktime(&value);
}

static void event(binbot_audio_calendar_state_t *snapshot, const char *id, const char *title,
                   time_t start, time_t end)
{
    binbot_audio_calendar_event_t *item = &snapshot->events[snapshot->event_count++];
    snprintf(item->event_id, sizeof(item->event_id), "%s", id);
    snprintf(item->summary, sizeof(item->summary), "%s", title);
    snprintf(item->start_timestamp, sizeof(item->start_timestamp), "%lld", (long long)start);
    snprintf(item->end_timestamp, sizeof(item->end_timestamp), "%lld", (long long)end);
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

static void settle(void)
{
    lv_tick_inc(50);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void click(ui_app_t *app, const char *text)
{
    lv_obj_t *found = label(app->current.root, text);
    assert(found);
    lv_obj_t *button = lv_obj_get_parent(found);
    while(button && !lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE)) button = lv_obj_get_parent(button);
    assert(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    tap(pointer_input, (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
}

static void pointer_sample(lv_indev_t *input, int x, int y, lv_indev_state_t state)
{
    pointer.point = (lv_point_t){x, y};
    pointer.state = state;
    lv_indev_read(input);
    settle();
}

static void tap(lv_indev_t *input, int x, int y)
{
    pointer_sample(input, x, y, LV_INDEV_STATE_PRESSED);
    pointer_sample(input, x, y, LV_INDEV_STATE_RELEASED);
}

static void drag(lv_indev_t *input, int x, int y, int dx, int dy)
{
    pointer_sample(input, x, y, LV_INDEV_STATE_PRESSED);
    for(int step = 1; step <= 12; ++step)
        pointer_sample(input, x + dx * step / 12, y + dy * step / 12, LV_INDEV_STATE_PRESSED);
    pointer_sample(input, x + dx, y + dy, LV_INDEV_STATE_RELEASED);
    for(unsigned i = 0; i < 7; ++i) settle();
}

static lv_obj_t *scroller(lv_obj_t *root)
{
    if(lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(root) == LV_DIR_VER) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = scroller(lv_obj_get_child(root, i));
        if(found) return found;
    }
    return NULL;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_CALENDAR_SNAPSHOT");
    if(!directory) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/calendar-%s.ppm", directory, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
    assert(frame && frame->header.w == 720 && frame->header.h == 720);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *p = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {p[2], p[1], p[0]};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file);
    lv_draw_buf_destroy(frame);
}

static void calendar_rules(void)
{
    app_calendar_t calendar = {0};
    binbot_audio_calendar_state_t input = {.revision = 1};
    event(&input, "a", "First", at(10, 17, 9, 0), at(10, 17, 10, 0));
    event(&input, "b", "Second", at(10, 17, 9, 30), at(10, 17, 10, 30));
    event(&input, "c", "Third", at(10, 17, 10, 0), at(10, 17, 11, 0));
    event(&input, "d", "Later", at(10, 17, 14, 0), at(10, 17, 15, 0));
    event(&input, "cross", "Cross day", at(10, 18, 22, 30), at(10, 19, 0, 30));
    event(&input, "midnight", "Midnight", at(10, 17, 23, 30), at(10, 18, 0, 0));
    binbot_audio_calendar_event_t *all = &input.events[input.event_count++];
    snprintf(all->event_id, sizeof(all->event_id), "all");
    snprintf(all->start_date, sizeof(all->start_date), "2026-10-19");
    snprintf(all->end_date, sizeof(all->end_date), "2026-10-22");
    assert(app_calendar_receive(&calendar, &input, current_time));
    assert(calendar.count == 7 && calendar.received);
    app_calendar_segment_t segments[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
    size_t count = app_calendar_segments(&calendar, at(10, 17, 0, 0), segments);
    assert(count == 5);
    assert(segments[0].columns == 2 && segments[1].columns == 2 && segments[2].columns == 2);
    assert(segments[0].column == segments[2].column && segments[3].columns == 1);
    count = app_calendar_segments(&calendar, at(10, 18, 0, 0), segments);
    assert(count == 1 && segments[0].start_minute == 1350 && segments[0].end_minute == 1440);
    assert(app_calendar_span(app_calendar_find(&calendar, "cross")) == 2);
    assert(app_calendar_span(app_calendar_find(&calendar, "midnight")) == 1);
    assert(app_calendar_span(app_calendar_find(&calendar, "all")) == 3);
    assert(app_calendar_segments(&calendar, at(10, 21, 0, 0), segments) == 1 && segments[0].band);
    assert(app_calendar_segments(&calendar, at(10, 22, 0, 0), segments) == 0);
    assert(!strcmp(app_calendar_next(&calendar, at(10, 17, 10, 0))->data.event_id, "b"));
    assert(app_calendar_set_connected(&calendar, true));
    assert(!app_calendar_set_connected(&calendar, true));
    assert(app_calendar_set_connected(&calendar, false) && calendar.count == 7);
    char text[96];
    app_calendar_when(text, sizeof(text), at(10, 18, 9, 0), at(10, 17, 23, 0));
    assert(!strcmp(text, "明天 09:00"));
    input.events[0].summary[0] = 'a';
    input.events[0].summary[1] = (char)0xe6;
    input.events[0].summary[2] = (char)0x97;
    input.events[0].summary[3] = 0;
    assert(app_calendar_receive(&calendar, &input, current_time));
    assert(!strcmp(app_calendar_find(&calendar, "a")->data.summary, "a"));
    input.event_count = BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY + 1;
    assert(!app_calendar_receive(&calendar, &input, current_time) && calendar.count == 7);
    input.event_count = 1;
    memset(input.events[0].summary, 'x', sizeof(input.events[0].summary));
    assert(!app_calendar_receive(&calendar, &input, current_time) && calendar.count == 7);
    input = (binbot_audio_calendar_state_t){.revision = 2};
    assert(app_calendar_receive(&calendar, &input, current_time) && calendar.count == 0 && calendar.received);
}

int main(void)
{
    setenv("TZ", "CST-8", 1); tzset();
    setenv("BINBOT_INITIAL_ROUTE", "calendar", 1);
    setenv("BINBOT_MODULE_IMAGE_ROOT", "/tmp/binbot-calendar-test-images", 1);
    setenv("BINBOT_WEATHER_URL", "http://127.0.0.1:1", 1);
    current_time = at(10, 17, 13, 40);
    calendar_rules();
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t pixels[720 * 64 * 4];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input_device = lv_indev_create(); assert(input_device);
    pointer_input = input_device;
    lv_indev_set_type(input_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(input_device, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(input_device, pointer_read);
    lv_indev_set_display(input_device, display);
    app_state_t *state = app_state_create(); assert(state);
    app_model_t before = *app_state_model(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    assert(label(app->current.root, "正在从飞书日历同步…"));
    snapshot(app, "loading");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE);
    binbot_audio_calendar_state_t input = {.revision = 7};
    event(&input, "t1", "测试日程 1：日程同步", at(10, 17, 9, 0), at(10, 17, 9, 20));
    event(&input, "t2", "测试日程 2：下午安排", at(10, 17, 14, 0), at(10, 17, 14, 30));
    event(&input, "t3", "测试日程 3：晚间安排", at(10, 17, 18, 0), at(10, 17, 18, 15));
    assert(ui_app_handle_calendar_state(app, &input)); settle();
    assert(label(app->current.root, "已同步"));
    snapshot(app, "month");
    drag(input_device, 570, 300, -350, 0);
    assert(app->calendar_day == at(11, 1, 0, 0) && app->route == UI_ROUTE_CALENDAR);
    drag(input_device, 210, 300, 350, 0);
    assert(app->calendar_day == at(10, 1, 0, 0));
    tap(input_device, 554, 51);
    assert(app->calendar_day == at(10, 17, 0, 0));
    drag(input_device, 440, 348, 0, -100);
    assert(app->calendar_folded);
    drag(input_device, 570, 198, -350, 0);
    assert(app->calendar_day == at(10, 24, 0, 0));
    drag(input_device, 210, 198, 350, 0);
    assert(app->calendar_day == at(10, 17, 0, 0));
    drag(input_device, 440, 178, 0, 44);
    assert(!app->calendar_folded);
    drag(input_device, 500, 542, -220, 0);
    assert(app->calendar_day == at(10, 18, 0, 0) && app->route == UI_ROUTE_CALENDAR);
    tap(input_device, 554, 51);
    assert(app->calendar_day == at(10, 17, 0, 0));
    click(app, "周视图"); assert(app->calendar_folded);
    snapshot(app, "week");
    drag(input_device, 570, 198, 30, 0);
    assert(app->calendar_day == at(10, 17, 0, 0));
    click(app, "月视图"); assert(!app->calendar_folded);
    click(app, "周视图"); assert(app->calendar_folded);
    lv_obj_t *day_scroll = scroller(app->current.root); assert(day_scroll);
    int initial_scroll = lv_obj_get_scroll_y(day_scroll);
    drag(input_device, 500, 550, 0, -120);
    assert(app->route == UI_ROUTE_CALENDAR && lv_obj_get_scroll_y(scroller(app->current.root)) > initial_scroll);
    int dragged_scroll = lv_obj_get_scroll_y(scroller(app->current.root));
    tap(input_device, 54, 51); assert(app->route == UI_ROUTE_HOME);
    click(app, "日程"); assert(app->route == UI_ROUTE_CALENDAR);
    assert(lv_obj_get_scroll_y(scroller(app->current.root)) == dragged_scroll);
    lv_obj_scroll_to_y(scroller(app->current.root), initial_scroll, LV_ANIM_OFF); settle();
    click(app, "测试日程 2：下午安排");
    assert(app->route == UI_ROUTE_CALENDAR_DETAIL && label(app->current.root, "还有 20 分钟"));
    snapshot(app, "detail");
    lv_obj_send_event(lv_obj_get_child(app->current.root, 1), LV_EVENT_CLICKED, NULL); settle();
    assert(app->route == UI_ROUTE_CALENDAR && app->calendar_folded);
    ui_app_navigate(app, UI_ROUTE_HOME); settle();
    assert(label(app->current.root, "下一场日程") && label(app->current.root, "14:00"));
    snapshot(app, "home");
    click(app, "下一场日程"); assert(app->route == UI_ROUTE_CALENDAR_DETAIL && app->calendar_from_home);
    lv_obj_send_event(lv_obj_get_child(app->current.root, 1), LV_EVENT_CLICKED, NULL); settle();
    assert(app->route == UI_ROUTE_HOME);
    ui_app_navigate(app, UI_ROUTE_CALENDAR); settle();
    app->calendar_day = at(10, 23, 0, 0); app->calendar_scroll_valid = false;
    ui_app_rebuild(app); settle(); assert(label(app->current.root, "这一天没有安排"));
    snapshot(app, "empty");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_DISCONNECTED); settle();
    assert(label(app->current.root, "离线") && app->calendar.count == 3);
    snapshot(app, "offline");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE); settle();
    current_time = at(10, 17, 14, 10);
    app->calendar_day = at(10, 17, 0, 0); app->calendar_scroll_valid = false;
    ui_app_rebuild(app); settle(); assert(label(app->current.root, "进行中"));
    snapshot(app, "live");
    current_time = at(10, 17, 14, 31);
    app->current.update(app->current.context, app_state_model(state), APP_CHANGE_CLOCK); settle();
    assert(!label(app->current.root, "进行中"));
    current_time = at(10, 17, 14, 10);
    ui_app_navigate(app, UI_ROUTE_CALENDAR_DETAIL); settle();
    input.event_count = 0; assert(ui_app_handle_calendar_state(app, &input)); settle();
    assert(label(app->current.root, "这条日程已不存在"));
    input = (binbot_audio_calendar_state_t){.revision = 8};
    event(&input, "long", "项目方案评审与跨团队协作讨论：确认下一阶段的交付计划和验收范围", at(10, 17, 14, 0), at(10, 17, 16, 0));
    snprintf(input.events[0].location, sizeof(input.events[0].location), "%s", "研发中心三层会议室，线上参会请使用会议链接，现场请提前签到");
    event(&input, "overlap", "产品讨论", at(10, 17, 14, 30), at(10, 17, 16, 0));
    event(&input, "cross", "跨天出差", at(10, 16, 9, 0), at(10, 18, 17, 0));
    binbot_audio_calendar_event_t *all = &input.events[input.event_count++];
    snprintf(all->event_id, sizeof(all->event_id), "all");
    snprintf(all->summary, sizeof(all->summary), "年度假期安排");
    snprintf(all->start_date, sizeof(all->start_date), "2026-10-17");
    snprintf(all->end_date, sizeof(all->end_date), "2026-10-19");
    assert(ui_app_handle_calendar_state(app, &input)); settle();
    ui_app_navigate(app, UI_ROUTE_CALENDAR); settle();
    snapshot(app, "edges");
    ui_calendar_open_event(app, "long", false); settle();
    snapshot(app, "long-detail");
    snprintf(input.events[0].summary, sizeof(input.events[0].summary), "%s", "议题一\n议题二\n议题三\n议题四\n议题五\n议题六\n议题七\n议题八");
    assert(ui_app_handle_calendar_state(app, &input)); settle();
    lv_obj_t *detail_scroll = scroller(app->current.root); assert(detail_scroll);
    lv_obj_scroll_to_y(detail_scroll, 75, LV_ANIM_OFF);
    int scroll_y = lv_obj_get_scroll_y(detail_scroll);
    assert(scroll_y > 0);
    ++current_time;
    current_time += 60;
    app->current.update(app->current.context, app_state_model(state), APP_CHANGE_CLOCK); settle();
    assert(lv_obj_get_scroll_y(scroller(app->current.root)) == scroll_y);
    ui_calendar_open_event(app, "all", false); settle();
    assert(label(app->current.root, "全天日程") && label(app->current.root, "共 2 天"));
    snapshot(app, "all-day-detail");
    assert(app_state_model(state)->todo_count == before.todo_count);
    assert(!memcmp(app_state_model(state)->todos, before.todos, sizeof(before.todos)));
    for(unsigned i = 0; i < 24; ++i) {
        ui_app_navigate(app, UI_ROUTE_CALENDAR); settle();
        ui_app_navigate(app, UI_ROUTE_HOME); settle();
    }
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    puts("Calendar boundaries, overlap layout, navigation, live data, offline preservation and clearing passed");
    return 0;
}
