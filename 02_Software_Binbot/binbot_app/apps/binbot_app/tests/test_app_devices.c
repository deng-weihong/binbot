#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lv_point_t point;
static lv_indev_state_t pointer_state;

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = point;
    data->state = pointer_state;
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

static void settle(void)
{
    lv_tick_inc(300);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void tap(lv_indev_t *input, int32_t x, int32_t y)
{
    point = (lv_point_t){x, y};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    settle();
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app)
{
    const char *path = getenv("BINBOT_WAKE_SNAPSHOT");
    if(!path) return;
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
    assert(frame && frame->header.w == 720 && frame->header.h == 720);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for(unsigned x = 0; x < 720; ++x) {
            uint8_t pixel[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            assert(fwrite(pixel, 1, 3, file) == 3);
        }
    }
    assert(!fclose(file));
    lv_draw_buf_destroy(frame);
}

int main(void)
{
    char directory[] = "/tmp/binbot-devices-ui-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "devices", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input = lv_indev_create(); assert(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read);
    lv_indev_set_display(input, display);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    settle();

    app_persisted_state_t before, after;
    app_state_export_persisted(state, &before);
    tap(input, 610, 240);
    assert(find_label(app->current.root, "开启台式机？"));
    assert(app_state_model(state)->computer == APP_COMPUTER_SLEEP);
    snapshot(app);
    app_state_tick(state, platform_monotonic_ms() + 5000);
    assert(app_state_model(state)->computer == APP_COMPUTER_SLEEP);
    tap(input, 210, 340);
    assert(find_label(app->current.root, "开启台式机？"));
    tap(input, 250, 440);
    assert(!find_label(app->current.root, "开启台式机？"));
    app_state_export_persisted(state, &after);
    assert(before.device_mask == after.device_mask && before.computer_state == after.computer_state);

    for(int i = 0; i < 5; ++i) {
        tap(input, 610, 240);
        assert(find_label(app->current.root, "开启台式机？"));
        tap(input, 60, 400);
        assert(!find_label(app->current.root, "开启台式机？"));
        assert(app_state_model(state)->computer == APP_COMPUTER_SLEEP);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    tap(input, 610, 240);
    tap(input, 460, 440);
    assert(!find_label(app->current.root, "开启台式机？"));
    assert(app_state_model(state)->computer == APP_COMPUTER_WAKING);
    assert(find_label(app->current.root, "开机中"));
    app_state_tick(state, platform_monotonic_ms() + 1500);
    assert(app_state_model(state)->computer == APP_COMPUTER_ONLINE);
    tap(input, 610, 240);
    assert(!find_label(app->current.root, "开启台式机？"));
    assert(find_label(app->current.root, "电脑正在运行。要让它休眠吗？"));

    app_state_sleep_computer(state);
    tap(input, 610, 240);
    assert(find_label(app->current.root, "开启台式机？"));
    ui_app_navigate(app, UI_ROUTE_ABOUT);
    settle();
    assert(app->route == UI_ROUTE_ABOUT);
    assert(!find_label(app->current.root, "开启台式机？"));
    assert(app_state_model(state)->computer == APP_COMPUTER_SLEEP);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_indev_delete(input);
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    assert(!rmdir(directory));
    puts("Desktop wake dialog: cancel, backdrop, panel, confirmation and route cleanup passed");
    return 0;
}
