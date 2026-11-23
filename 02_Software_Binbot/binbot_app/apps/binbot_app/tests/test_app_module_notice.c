#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static binbot_component_event_t report(ui_app_t *app, unsigned profile, bool healthy)
{
    static uint64_t sequence;
    binbot_component_event_t event = {.status = BINBOT_COMPONENT_PROBED,
        .has_info = profile != 0, .profile = profile, .healthy = healthy, .event_seq = ++sequence};
    app_modules_handle_event(app->modules, &event);
    return event;
}

static lv_obj_t *banner(ui_app_t *app, const char *text)
{
    lv_obj_t *label = find_label(app->root, text); assert(label);
    lv_obj_t *root = lv_obj_get_parent(label);
    assert(lv_obj_get_parent(root) == app->root);
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_CLICKABLE));
    return root;
}

static void pulse(int fd)
{
    binbot_slave_ipc_packet_t packet;
    ssize_t size = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(size > 0 && binbot_slave_ipc_packet_is_valid(&packet, (size_t)size));
    assert(packet.type == BINBOT_SLAVE_IPC_MSG_REQUEST);
    assert(packet.command == BINBOT_SLAVE_CMD_MOTOR_PULSE && packet.request_id != 0);
    assert(packet.payload_length == 2 && packet.payload[0] == 50 && packet.payload[1] == 0);
}

static void no_pulse(int fd)
{
    binbot_slave_ipc_packet_t packet;
    assert(recv(fd, &packet, sizeof(packet), MSG_DONTWAIT) < 0);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_HOTPLUG_SNAPSHOT_DIR");
    if(!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
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
    char directory[] = "/tmp/binbot-module-notice-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "devices", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app && app->module_notice);
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    binbot_slave_client_t client;
    binbot_slave_client_init(&client, BINBOT_SLAVE_IPC_ROLE_APP, NULL);
    client.fd = sockets[0];
    ui_app_set_slave_client(app, &client);
    assert(app->firmware.client == &client);
    assert(!ui_app_vibrate(app, 0) && !ui_app_vibrate(app, 5001));
    no_pulse(sockets[1]);
    advance(100);
    report(app, 5, true);
    pulse(sockets[1]);
    advance(400);
    lv_obj_t *root = banner(app, "组件已插入");
    assert(find_label(root, "已识别：氛围灯"));
    assert(lv_obj_get_x(root) == 150 && lv_obj_get_y(root) == 22);
    assert(lv_obj_get_width(root) == 420 && lv_obj_get_height(root) == 74);
    snapshot(app, "hotplug-inserted-native");
    advance(1600);
    report(app, 5, false);
    report(app, 5, true);
    no_pulse(sockets[1]);
    advance(601);
    assert(!find_label(app->root, "组件已插入"));

    binbot_component_event_t missing = report(app, 0, false);
    for(unsigned i = 0; i < 5; ++i) app_modules_handle_event(app->modules, &missing);
    assert(!find_label(app->root, "组件已拔出"));
    report(app, 5, true);
    report(app, 0, false);
    assert(!find_label(app->root, "组件已拔出"));
    no_pulse(sockets[1]);
    report(app, 0, false);
    pulse(sockets[1]);
    advance(400);
    root = banner(app, "组件已拔出");
    assert(find_label(root, "氛围灯已断开"));
    snapshot(app, "hotplug-removed-native");
    ui_app_navigate(app, UI_ROUTE_ABOUT);
    advance(300);
    assert(banner(app, "组件已拔出") == root);
    advance(2000);
    assert(!find_label(app->root, "组件已拔出"));

    static const ui_route_t routes[] = {
        UI_ROUTE_HOME, UI_ROUTE_APPS, UI_ROUTE_MUSIC, UI_ROUTE_ALARM,
        UI_ROUTE_POMODORO, UI_ROUTE_VOCABULARY, UI_ROUTE_TODO, UI_ROUTE_DEVICES,
        UI_ROUTE_ASSISTANT, UI_ROUTE_CAMERA, UI_ROUTE_SETTINGS, UI_ROUTE_WIFI,
        UI_ROUTE_LUNCH, UI_ROUTE_MODULES, UI_ROUTE_ABOUT,
    };
    static const unsigned profiles[] = {1, 2, 3, 5};
    static const char *inserted[] = {"已识别：笔筒", "已识别：全息屏", "已识别：转盘", "已识别：氛围灯"};
    static const char *removed[] = {"笔筒已断开", "全息屏已断开", "转盘已断开", "氛围灯已断开"};
    for(unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ui_app_navigate(app, routes[i]); advance(300);
        assert(app->route == routes[i]);
        report(app, profiles[i % 4], true); advance(400);
        pulse(sockets[1]);
        root = banner(app, "组件已插入");
        assert(find_label(root, inserted[i % 4]));
        report(app, 4, true);
        no_pulse(sockets[1]);
        assert(banner(app, "组件已插入") == root);
        report(app, 0, false); report(app, 0, false);
        advance(501); advance(400);
        pulse(sockets[1]);
        root = banner(app, "组件已拔出");
        assert(find_label(root, removed[i % 4]));
        advance(2201);
        assert(!find_label(app->root, "组件已拔出"));
    }

    report(app, 1, true); pulse(sockets[1]); advance(2601);
    report(app, 2, true); advance(400);
    pulse(sockets[1]);
    assert(find_label(banner(app, "组件已拔出"), "笔筒已断开"));
    advance(501); advance(400);
    pulse(sockets[1]);
    root = banner(app, "组件已插入");
    assert(find_label(root, "已识别：全息屏"));
    ui_app_open_control_center(app); advance(400);
    assert(lv_obj_get_child(app->root, -1) == root);
    assert(find_label(root, "组件已插入"));
    ui_app_close_control_center(app);
    ui_app_navigate(app, UI_ROUTE_SETTINGS); advance(300);
    assert(banner(app, "组件已插入") == root);
    report(app, 3, true);
    assert(lv_mem_test() == LV_RESULT_OK);
    ui_app_destroy(app); app_state_destroy(state);
    advance(5000);
    no_pulse(sockets[1]);
    binbot_slave_client_close(&client); close(sockets[1]);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    assert(!rmdir(directory));
    puts("Global component notices: all routes, debounce, expiry, replacement, control center and cleanup passed");
    return 0;
}
