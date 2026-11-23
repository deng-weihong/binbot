#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *find_slider(lv_obj_t *root)
{
    if(lv_obj_check_type(root, &lv_slider_class)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_slider(lv_obj_get_child(root, i));
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

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app)
{
    const char *path = getenv("BINBOT_MOTOR_SNAPSHOT");
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
    char root[] = "/tmp/binbot-motor-ui-XXXXXX"; assert(mkdtemp(root));
    setenv("BINBOT_MODULE_IMAGE_ROOT", root, 1);
    setenv("BINBOT_COMPONENT_STATE_DIR", root, 1);
    setenv("BINBOT_COMPONENT_UART", "/nonexistent-binbot-motor-ui-uart", 1);
    setenv("BINBOT_INITIAL_ROUTE", "modules", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    binbot_component_event_t event = {.has_info = 1, .profile = 3, .healthy = 1,
        .status = BINBOT_COMPONENT_PROBED, .motor_supported = 1, .motor_speed = 40};
    app_modules_handle_event(app->modules, &event);
    lv_obj_t *label = find_label(app->current.root, "转盘"); assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    settle();
    assert(find_label(app->current.root, "已停止"));
    lv_obj_t *slider = find_slider(app->current.root); assert(slider);
    assert(lv_slider_get_value(slider) == 40 && !lv_obj_has_state(slider, LV_STATE_DISABLED));
    snapshot(app);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    worker.pid = 1; worker.active_action = BINBOT_COMPONENT_PROBE;
    app_modules_set_worker(app->modules, &worker);
    label = find_label(app->current.root, "顺时针"); assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    assert(app_modules_motor_busy(app->modules));
    assert(find_label(app->current.root, "正在设置…"));
    slider = find_slider(app->current.root);
    assert(lv_obj_has_state(slider, LV_STATE_DISABLED));
    worker.pid = 0;
    app_modules_tick(app->modules, 2000);
    memcpy(event.request_id, worker.latest.request_id, 16);
    bb_component_worker_close(&worker);
    event.status = BINBOT_COMPONENT_SUCCEEDED; event.motor_direction = 2;
    app_modules_handle_event(app->modules, &event);
    settle();
    assert(find_label(app->current.root, "顺时针旋转"));
    slider = find_slider(app->current.root); assert(slider);
    worker.pid = 1; worker.active_action = BINBOT_COMPONENT_PROBE;
    lv_slider_set_value(slider, 75, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(find_label(app->current.root, "75%"));
    assert(!app_modules_motor_busy(app->modules));
    lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
    assert(app_modules_motor_busy(app->modules));
    worker.pid = 0;
    app_modules_tick(app->modules, 3000);
    memcpy(event.request_id, worker.latest.request_id, 16);
    bb_component_worker_close(&worker);
    event.motor_speed = 75;
    app_modules_handle_event(app->modules, &event);
    settle();
    assert(lv_slider_get_value(find_slider(app->current.root)) == 75);
    event.motor_supported = 0; event.status = BINBOT_COMPONENT_PROBED;
    app_modules_handle_event(app->modules, &event);
    settle();
    assert(find_label(app->current.root, "请升级转盘固件"));
    assert(lv_obj_has_state(find_slider(app->current.root), LV_STATE_DISABLED));
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    char lock[256]; snprintf(lock, sizeof(lock), "%s/worker.lock", root); unlink(lock);
    assert(!rmdir(root));
    puts("motor UI direction, slider, acknowledgement and old-firmware states passed");
    return 0;
}
