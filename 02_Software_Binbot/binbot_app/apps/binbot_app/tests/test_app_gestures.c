#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "binbot/app_runtime.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t test_now = 100000;
static uint64_t sequence;
static unsigned slow_flush_ms;
static lv_indev_data_t pointer_data;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev; *data = pointer_data;
}

int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
    if(clock != CLOCK_MONOTONIC) return __real_clock_gettime(clock, value);
    *value = (struct timespec){.tv_sec = test_now / 1000, .tv_nsec = (test_now % 1000) * 1000000};
    return 0;
}

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void advance(unsigned ms)
{
    while(ms) {
        unsigned step = ms > 50 ? 50 : ms;
        test_now += step; lv_tick_inc(step); lv_timer_handler(); ms -= step;
    }
    lv_obj_update_layout(lv_screen_active());
}

static void settle(void) { advance(120); }

static void drag(int x, int from, int to)
{
    pointer_data.point = (lv_point_t){x, from};
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    advance(50);
    for(int i = 1; i <= 12; ++i) {
        pointer_data.point.y = from + (to - from) * i / 12;
        advance(50);
    }
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    advance(400);
}

static void click(lv_obj_t *root, const char *text)
{
    lv_obj_t *label = find(root, text); assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL); settle();
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    test_now += slow_flush_ms; slow_flush_ms = 0;
    lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *folder = getenv("BINBOT_GESTURE_SNAPSHOT_DIR");
    if(!folder) return;
    lv_obj_update_layout(app->root);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for(unsigned x = 0; x < 720; ++x) {
            uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            assert(fwrite(rgb, 1, 3, file) == 3);
        }
    }
    assert(!fclose(file)); lv_draw_buf_destroy(frame);
}

static void feed(ui_app_t *app, binbot_gesture_label_t label, float score, unsigned frames)
{
    for(unsigned i = 0; i < frames; ++i) {
        test_now += 200;
        binbot_hand_result_t frame = {.sequence = ++sequence, .monotonic_ms = test_now,
                                       .gesture = {.label = label, .score = score}};
        ui_gesture_receive(app, &frame);
    }
}

static void test_persistence(void)
{
    char path[] = "/tmp/binbot-gestures-state-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "w"); assert(file);
    fputs("schema=4\nbrightness=41\nvolume=53\ncontrols=7\nalarm_count=0\ntodo_count=0\n", file);
    fclose(file);
    app_runtime_t *runtime = app_runtime_create(path); assert(runtime);
    app_state_t *state = app_runtime_state(runtime);
    app_gesture_settings_t s = app_state_model(state)->gestures;
    assert(s.enabled && s.confirm_sensitive && s.sensitivity == 1 && s.enabled_mask == 0x1b);
    assert(s.actions[APP_GESTURE_OK] == APP_GESTURE_PC_ON);
    s.enabled = false; s.sensitivity = 2; s.enabled_mask = 3;
    s.actions[APP_GESTURE_OK] = APP_GESTURE_FOCUS;
    assert(app_state_set_gestures(state, &s));
    app_gesture_settings_t invalid = s; invalid.actions[0] = 255;
    assert(!app_state_set_gestures(state, &invalid));
    invalid = s; invalid.sensitivity = 3; assert(!app_state_set_gestures(state, &invalid));
    app_runtime_flush(runtime); app_runtime_destroy(runtime);
    runtime = app_runtime_create(path); assert(runtime);
    const app_model_t *m = app_state_model(app_runtime_state(runtime));
    assert(!m->gestures.enabled && m->gestures.sensitivity == 2 && m->gestures.enabled_mask == 3);
    assert(m->gestures.actions[0] == APP_GESTURE_FOCUS && m->brightness == 41 && m->volume == 53);
    app_runtime_destroy(runtime); assert(!unlink(path));
}

int main(void)
{
    test_persistence();
    setenv("BINBOT_GESTURE_CAPTURE", "0", 1);
    setenv("BINBOT_INITIAL_ROUTE", "settings", 1);
    char directory[] = "/tmp/binbot-gestures-ui-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    lv_indev_t *pointer = lv_indev_create(); assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    settle(); snapshot(app, "settings-top");
    lv_obj_t *content = lv_obj_get_parent(lv_obj_get_parent(find(app->current.root, "关于本机")));
    assert(lv_obj_get_scroll_y(content) == 0);
    drag(300, 610, 260);
    /* The restored two-column settings layout fits in one screen. */
    assert(lv_obj_get_scroll_y(content) == 0 && app->route == UI_ROUTE_SETTINGS);
    snapshot(app, "settings-dragged");
    drag(300, 300, 620);
    assert(lv_obj_get_scroll_y(content) == 0);
    lv_obj_t *about = find(app->current.root, "关于本机"); assert(about);
    lv_obj_scroll_to_view(lv_obj_get_parent(about), LV_ANIM_OFF); settle();
    snapshot(app, "settings-bottom");
    click(app->current.root, "手势设置");
    assert(app->route == UI_ROUTE_GESTURES);
    snapshot(app, "default");
    click(app->current.root, "试一试");
    assert(find(app->root, "确认") && find(app->root, "识别到手势")); snapshot(app, "confirmation");
    click(app->root, "取消"); assert(!find(app->root, "确认") && find(app->root, "已取消"));
    snapshot(app, "cancelled"); advance(1400); assert(!ui_gesture_popup_active(app->gestures));
    click(app->current.root, "执行");
    assert(find(app->current.root, "「OK」执行什么？")); snapshot(app, "actions");
    click(app->current.root, "开始专注");
    assert(app_state_model(state)->gestures.actions[0] == APP_GESTURE_FOCUS);
    feed(app, BINBOT_GESTURE_NONE, 0, 3);
    feed(app, BINBOT_GESTURE_OK, 0.99f, 5); assert(!app->route_pending);
    feed(app, BINBOT_GESTURE_OK, 0.99f, 1);
    assert(find(app->root, "即将开始专注") && !app->route_pending);
    snapshot(app, "countdown");
    advance(2000); assert(app->route == UI_ROUTE_GESTURES);
    advance(300); assert(app->route == UI_ROUTE_POMODORO && app_state_model(state)->pomodoro.running);
    assert(find(app->root, "已开始专注") && ui_gesture_popup_active(app->gestures));
    snapshot(app, "done"); advance(1400); assert(!ui_gesture_popup_active(app->gestures));
    app_state_pause_pomodoro(state);
    feed(app, BINBOT_GESTURE_OK, 0.99f, 12);
    assert(!app_state_model(state)->pomodoro.running);
    feed(app, BINBOT_GESTURE_NONE, 0, 3);
    feed(app, BINBOT_GESTURE_OK, 0.3f, 8); assert(!app_state_model(state)->pomodoro.running);
    feed(app, BINBOT_GESTURE_OK, 0.99f, 6); settle();
    assert(!app_state_model(state)->pomodoro.running && find(app->root, "即将开始专注"));
    click(app->root, "取消"); advance(2500);
    assert(!app_state_model(state)->pomodoro.running);
    feed(app, BINBOT_GESTURE_NONE, 0, 3); feed(app, BINBOT_GESTURE_OK, 0.99f, 6);
    advance(2300);
    assert(app_state_model(state)->pomodoro.running);
    advance(1400);

    app_gesture_settings_t s = app_state_model(state)->gestures;
    s.actions[0] = APP_GESTURE_PC_SLEEP;
    assert(app_state_set_gestures(state, &s));
    feed(app, BINBOT_GESTURE_OK, 0.99f, 10); assert(!find(app->root, "确认"));
    feed(app, BINBOT_GESTURE_NONE, 0, 3); feed(app, BINBOT_GESTURE_OK, 0.99f, 6);
    assert(find(app->root, "确认"));
    s.enabled = false; assert(app_state_set_gestures(state, &s)); settle();
    assert(!find(app->root, "确认"));
    feed(app, BINBOT_GESTURE_OK, 0.99f, 12); assert(!find(app->root, "确认"));
    ui_app_navigate(app, UI_ROUTE_GESTURES); settle(); snapshot(app, "disabled");
    s.enabled = true; s.sensitivity = 2; assert(app_state_set_gestures(state, &s));
    click(app->current.root, "张开手掌"); snapshot(app, "palm");
    click(app->current.root, "点赞"); snapshot(app, "thumb");
    click(app->current.root, "比耶"); snapshot(app, "victory");
    click(app->current.root, "握拳"); snapshot(app, "fist");
    lv_obj_t *back = lv_obj_get_child(app->current.root, 1);
    lv_obj_send_event(back, LV_EVENT_CLICKED, NULL); settle(); assert(app->route == UI_ROUTE_SETTINGS);

    /* The root overlay survives navigation and is available on every route. */
    for(ui_route_t route = UI_ROUTE_HOME; route <= UI_ROUTE_GESTURES; ++route) {
        ui_app_navigate(app, route); settle();
        assert(app->route == route);
        ui_screensaver_show(app->screensaver); assert(ui_screensaver_active(app->screensaver));
        feed(app, BINBOT_GESTURE_NONE, 0, 3); feed(app, BINBOT_GESTURE_OK, 0.99f, 4);
        assert(!ui_screensaver_active(app->screensaver) && find(app->root, "是否电脑休眠？"));
        lv_obj_t *popup = lv_obj_get_child(app->root, -1);
        assert(lv_obj_get_parent(popup) == app->root);
        ui_app_navigate(app, UI_ROUTE_HOME); settle();
        assert(ui_gesture_popup_active(app->gestures) && lv_obj_get_child(app->root, -1) == popup);
        assert(!lv_obj_has_flag(popup, LV_OBJ_FLAG_EVENT_BUBBLE));
        lv_obj_send_event(lv_obj_get_child(popup, -1), LV_EVENT_CLICKED, NULL);
        assert(find(app->root, "确认"));
        ui_app_open_control_center(app);
        assert(!app->control_center || !ui_control_center_is_active(app->control_center));
        click(app->root, "取消"); advance(1400);
        assert(!ui_gesture_popup_active(app->gestures));
    }
    ui_app_open_control_center(app); settle();
    assert(app->control_center && ui_control_center_is_active(app->control_center));
    ui_gesture_try(app, APP_GESTURE_OK);
    assert(ui_gesture_popup_active(app->gestures) && !ui_control_center_is_active(app->control_center));
    click(app->root, "确认");
    assert(find(app->root, "未执行") && find(app->root, "电脑控制服务尚未接入"));
    snapshot(app, "unavailable"); advance(1400);
    ui_gesture_try(app, APP_GESTURE_OK); advance(15100);
    assert(find(app->root, "确认已超时") && !find(app->root, "确认")); advance(1400);

    s.actions[0] = APP_GESTURE_FOCUS; assert(app_state_set_gestures(state, &s));
    app_state_pause_pomodoro(state);
    ui_gesture_try(app, APP_GESTURE_OK);
    s.enabled = false; assert(app_state_set_gestures(state, &s));
    advance(3000); assert(!app_state_model(state)->pomodoro.running);
    s.enabled = true; assert(app_state_set_gestures(state, &s));
    ui_gesture_try(app, APP_GESTURE_OK);
    slow_flush_ms = 3000; settle();
    assert(find(app->root, "即将开始专注") && !app_state_model(state)->pomodoro.running);
    advance(1900); assert(!app_state_model(state)->pomodoro.running);
    click(app->root, "取消"); advance(1400);
    s.enabled = true; s.actions[0] = APP_GESTURE_PHOTO; assert(app_state_set_gestures(state, &s));
    ui_gesture_try(app, APP_GESTURE_OK); advance(2300);
    assert(app->route == UI_ROUTE_CAMERA && find(app->root, "正在等待相机画面"));
    advance(5100); assert(find(app->root, "相机不可用，未拍摄照片"));
    snapshot(app, "photo-unavailable"); advance(1400);
    s.actions[0] = APP_GESTURE_PC_ON; assert(app_state_set_gestures(state, &s));
    ui_app_navigate(app, UI_ROUTE_ABOUT);
    ui_gesture_try(app, APP_GESTURE_OK); settle();
    assert(app->route == UI_ROUTE_ABOUT && find(app->root, "是否打开电脑？"));
    click(app->root, "取消"); advance(1400);
    ui_app_set_control_center_drag_active(app, true);
    ui_gesture_try(app, APP_GESTURE_OK);
    assert(!app->content_frozen && find(app->root, "确认"));
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    assert(!rmdir(directory));
    puts("Gesture popup, countdown/cancel, all routes/screensavers, scrolling, persistence and stabilization checks passed");
    return 0;
}
