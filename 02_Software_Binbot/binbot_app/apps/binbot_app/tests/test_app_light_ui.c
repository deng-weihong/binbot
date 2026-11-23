#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *slider(lv_obj_t *root)
{
    if(lv_obj_check_type(root, &lv_slider_class)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = slider(lv_obj_get_child(root, i));
        if(found) return found;
    }
    return NULL;
}

static void settle(void)
{
    lv_tick_inc(300); lv_timer_handler(); lv_obj_update_layout(lv_screen_active());
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void click(ui_app_t *app, const char *text)
{
    lv_obj_t *found = label(app->current.root, text); assert(found);
    lv_obj_send_event(lv_obj_get_parent(found), LV_EVENT_CLICKED, NULL);
    settle();
}

static void snapshot(ui_app_t *app)
{
    const char *path = getenv("BINBOT_LIGHT_SNAPSHOT"); if(!path) return;
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for(unsigned x = 0; x < 720; ++x) {
            const uint8_t pixel[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            assert(fwrite(pixel, 1, 3, file) == 3);
        }
    }
    fclose(file); lv_draw_buf_destroy(frame);
}

static void confirm(ui_app_t *app, bb_component_worker_t *worker, binbot_component_event_t *event)
{
    assert(worker->command_pending);
    memcpy(event->request_id, worker->pending_command.request_id, 16);
    worker->command_pending = false;
    event->event_seq++; event->status = BINBOT_COMPONENT_SUCCEEDED;
    app_modules_handle_event(app->modules, event); settle();
    assert(!app_modules_light_busy(app->modules));
}

int main(void)
{
    char root[] = "/tmp/binbot-light-ui-XXXXXX"; assert(mkdtemp(root));
    setenv("BINBOT_MODULE_IMAGE_ROOT", root, 1); setenv("BINBOT_COMPONENT_STATE_DIR", root, 1);
    setenv("BINBOT_COMPONENT_UART", "/nonexistent-binbot-light-uart", 1);
    setenv("BINBOT_INITIAL_ROUTE", "modules", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); ui_app_t *app = ui_app_create(state); assert(app);
    binbot_component_event_t event = {.has_info = 1, .profile = BB_COMPONENT_AMBIENT_LIGHT,
        .healthy = 1, .boot_id = 42, .status = BINBOT_COMPONENT_PROBED, .event_seq = 1};
    strcpy(event.message, "{\"light\":{\"supported\":false}}");
    app_modules_handle_event(app->modules, &event);
    click(app, "氛围灯");
    assert(label(app->current.root, "请升级灯组件固件"));
    assert(lv_obj_has_state(slider(app->current.root), LV_STATE_DISABLED));
    strcpy(event.message, "{\"light\":{\"supported\":true,\"on\":1,\"fx\":0,\"r\":232,\"g\":100,\"b\":60,\"v\":70,\"sp\":50,\"off\":0,\"left\":0}}");
    event.event_seq++; app_modules_handle_event(app->modules, &event); settle();
    assert(app_modules_light_supported(app->modules));
    assert(label(app->current.root, "已开启") && label(app->current.root, "亮度 70%"));
    snapshot(app);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    worker.pid = 1; worker.active_action = BINBOT_COMPONENT_PROBE;
    app_modules_set_worker(app->modules, &worker);
    click(app, "已开启");
    assert(app_modules_light_busy(app->modules) && label(app->current.root, "正在设置…"));
    assert(app_modules_light_state(app->modules)->enabled);
    bb_component_light_settings_t pending;
    assert(bb_component_light_decode((uint8_t *)worker.pending_command.file_name, BB_COMPONENT_LIGHT_BYTES, &pending));
    assert(!pending.enabled && worker.pending_command.before_boot_id == 42);
    strcpy(event.message, "{\"light\":{\"supported\":true,\"on\":0,\"fx\":0,\"r\":232,\"g\":100,\"b\":60,\"v\":70,\"sp\":50,\"off\":0,\"left\":0}}");
    confirm(app, &worker, &event); assert(label(app->current.root, "已关闭"));
    click(app, "呼吸");
    assert(bb_component_light_decode((uint8_t *)worker.pending_command.file_name, BB_COMPONENT_LIGHT_BYTES, &pending));
    assert(pending.enabled && pending.effect == BB_COMPONENT_LIGHT_BREATHE);
    strcpy(event.message, "{\"light\":{\"supported\":true,\"on\":1,\"fx\":1,\"r\":232,\"g\":100,\"b\":60,\"v\":70,\"sp\":50,\"off\":0,\"left\":0}}");
    confirm(app, &worker, &event);
    lv_obj_t *control = slider(app->current.root); assert(!lv_obj_has_state(control, LV_STATE_DISABLED));
    lv_slider_set_value(control, 35, LV_ANIM_OFF); lv_obj_send_event(control, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!app_modules_light_busy(app->modules)); assert(label(app->current.root, "亮度 35%"));
    lv_obj_send_event(control, LV_EVENT_RELEASED, NULL);
    assert(bb_component_light_decode((uint8_t *)worker.pending_command.file_name, BB_COMPONENT_LIGHT_BYTES, &pending));
    assert(pending.brightness == 35 && app_modules_light_state(app->modules)->brightness == 70);
    strcpy(event.message, "{\"light\":{\"supported\":true,\"on\":1,\"fx\":1,\"r\":232,\"g\":100,\"b\":60,\"v\":35,\"sp\":50,\"off\":0,\"left\":0}}");
    confirm(app, &worker, &event);
    click(app, "定时");
    assert(bb_component_light_decode((uint8_t *)worker.pending_command.file_name, BB_COMPONENT_LIGHT_BYTES, &pending));
    assert(pending.timer_minutes == 15);
    strcpy(event.message, "{\"light\":{\"supported\":true,\"on\":1,\"fx\":1,\"r\":232,\"g\":100,\"b\":60,\"v\":35,\"sp\":50,\"off\":15,\"left\":15}}");
    confirm(app, &worker, &event); assert(label(app->current.root, "15 分钟后关灯"));
    uint32_t revision = app_modules_revision(app->modules);
    event.boot_id++; event.status = BINBOT_COMPONENT_PROBED; event.event_seq++;
    strcpy(event.message, "{}");
    app_modules_handle_event(app->modules, &event); settle();
    assert(app_modules_revision(app->modules) > revision && !app_modules_light_supported(app->modules));
    assert(lv_obj_has_state(slider(app->current.root), LV_STATE_DISABLED));
    event.has_info = 0; event.status = BINBOT_COMPONENT_PROBED; event.event_seq++;
    app_modules_handle_event(app->modules, &event); settle();
    assert(!app_modules_light_supported(app->modules)); assert(label(app->current.root, "氛围灯未插入"));
    worker.pid = 0; bb_component_worker_close(&worker);
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    assert(!rmdir(root));
    puts("light UI state, acknowledgement, effects, sliders, timer and disconnect passed");
    return 0;
}
