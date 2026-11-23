#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <dbus/dbus.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

static const char *paths[] = {"/org/bluez/hci0/dev_AA_AA_AA_AA_AA_AA",
    "/org/bluez/hci0/dev_BB_BB_BB_BB_BB_BB", "/org/bluez/hci0/dev_CC_CC_CC_CC_CC_CC"};
static const char *names[] = {"小林的 iPhone", "MacBook Air", "Xiaomi 14"};
static bool powered = true, discoverable = true, playing = true, paired[] = {true, true, true};
static int connected;
static dbus_uint16_t volume = 70;
static unsigned song;
static const char *titles[] = {"晴天", "Lemon", "这是一首名称很长的歌曲用来检查蓝牙页面中的曲目信息是否正确省略显示", ""};
static const char *artists[] = {"周杰伦", "米津玄师", "一位名称很长的歌手", ""};
static atomic_bool stop;
static atomic_uint calls;
static DBusConnection *service;

static void prop(DBusMessageIter *props, const char *name, int type, const void *value)
{
    DBusMessageIter entry, variant; char signature[] = {(char)type, 0};
    assert(dbus_message_iter_open_container(props, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
    assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
    assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant));
    assert(dbus_message_iter_append_basic(&variant, type, value));
    assert(dbus_message_iter_close_container(&entry, &variant));
    assert(dbus_message_iter_close_container(props, &entry));
}

static void str_prop(DBusMessageIter *props, const char *name, const char *value)
{ prop(props, name, DBUS_TYPE_STRING, &value); }
static void bool_prop(DBusMessageIter *props, const char *name, bool value)
{ dbus_bool_t boolean = value; prop(props, name, DBUS_TYPE_BOOLEAN, &boolean); }

static void object(DBusMessageIter *objects, const char *path, unsigned kind, unsigned index)
{
    DBusMessageIter entry, interfaces, interface, props;
    const char *name = kind == 0 ? "org.bluez.Adapter1" : kind == 1 ? "org.bluez.Device1" :
                         kind == 2 ? "org.bluez.MediaPlayer1" : "org.bluez.MediaTransport1";
    assert(dbus_message_iter_open_container(objects, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
    assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH, &path));
    assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sa{sv}}", &interfaces));
    assert(dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &interface));
    assert(dbus_message_iter_append_basic(&interface, DBUS_TYPE_STRING, &name));
    assert(dbus_message_iter_open_container(&interface, DBUS_TYPE_ARRAY, "{sv}", &props));
    if(kind == 0) {
        str_prop(&props, "Alias", "小伴 XiaoBan-S1");
        bool_prop(&props, "Powered", powered); bool_prop(&props, "Discoverable", discoverable);
    }
    else if(kind == 1) {
        str_prop(&props, "Alias", names[index]); str_prop(&props, "Icon", index == 1 ? "computer" : "phone");
        bool_prop(&props, "Paired", paired[index]); bool_prop(&props, "Connected", connected == (int)index && powered);
    }
    else if(kind == 2) {
        str_prop(&props, "Status", playing ? "playing" : "paused");
        DBusMessageIter track_entry, variant, track;
        const char *key = "Track";
        assert(dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, NULL, &track_entry));
        assert(dbus_message_iter_append_basic(&track_entry, DBUS_TYPE_STRING, &key));
        assert(dbus_message_iter_open_container(&track_entry, DBUS_TYPE_VARIANT, "a{sv}", &variant));
        assert(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "{sv}", &track));
        if(song != 3) { str_prop(&track, "Title", titles[song]); str_prop(&track, "Artist", artists[song]); }
        assert(dbus_message_iter_close_container(&variant, &track));
        assert(dbus_message_iter_close_container(&track_entry, &variant));
        assert(dbus_message_iter_close_container(&props, &track_entry));
    }
    else {
        str_prop(&props, "State", playing ? "active" : "idle");
        unsigned char codec = 2; prop(&props, "Codec", DBUS_TYPE_BYTE, &codec);
        prop(&props, "Volume", DBUS_TYPE_UINT16, &volume);
    }
    assert(dbus_message_iter_close_container(&interface, &props));
    assert(dbus_message_iter_close_container(&interfaces, &interface));
    assert(dbus_message_iter_close_container(&entry, &interfaces));
    assert(dbus_message_iter_close_container(objects, &entry));
}

static void *server(void *unused)
{
    (void)unused;
    while(!atomic_load(&stop)) {
        dbus_connection_read_write(service, 10);
        DBusMessage *message = dbus_connection_pop_message(service);
        if(!message) continue;
        if(dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) { dbus_message_unref(message); continue; }
        const char *method = dbus_message_get_member(message), *path = dbus_message_get_path(message);
        DBusMessage *reply = dbus_message_new_method_return(message); assert(reply);
        if(!strcmp(method, "GetManagedObjects")) {
            DBusMessageIter args, objects;
            dbus_message_iter_init_append(reply, &args);
            assert(dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects));
            object(&objects, "/org/bluez/hci0", 0, 0);
            for(unsigned i = 0; i < 3; ++i) if(paired[i]) object(&objects, paths[i], 1, i);
            if(connected >= 0 && powered) {
                char media[256]; snprintf(media, sizeof(media), "%s/player0", paths[connected]); object(&objects, media, 2, 0);
                snprintf(media, sizeof(media), "%s/fd0", paths[connected]); object(&objects, media, 3, 0);
            }
            assert(dbus_message_iter_close_container(&args, &objects));
        }
        else {
            atomic_fetch_add(&calls, 1);
            if(!strcmp(method, "Set")) {
                DBusMessageIter args, variant; const char *interface, *property;
                dbus_message_iter_init(message, &args); dbus_message_iter_get_basic(&args, &interface);
                dbus_message_iter_next(&args); dbus_message_iter_get_basic(&args, &property);
                dbus_message_iter_next(&args); dbus_message_iter_recurse(&args, &variant);
                if(!strcmp(property, "Volume")) dbus_message_iter_get_basic(&variant, &volume);
                else {
                    dbus_bool_t enabled; dbus_message_iter_get_basic(&variant, &enabled);
                    if(!strcmp(property, "Powered")) { powered = enabled; if(!powered) connected = -1; }
                    if(!strcmp(property, "Discoverable")) discoverable = enabled;
                }
            }
            else if(!strcmp(method, "Connect")) {
                usleep(800000);
                if(!strcmp(path, paths[1])) {
                    dbus_message_unref(reply);
                    reply = dbus_message_new_error(message, "org.bluez.Error.Failed", "Connection refused");
                }
                else for(unsigned i = 0; i < 3; ++i) if(!strcmp(path, paths[i])) { connected = (int)i; playing = true; }
            }
            else if(!strcmp(method, "Disconnect")) connected = -1;
            else if(!strcmp(method, "Pause")) playing = false;
            else if(!strcmp(method, "Play")) playing = true;
            else if(!strcmp(method, "Previous") || !strcmp(method, "Next")) {
                assert(dbus_message_has_interface(message, "org.bluez.MediaPlayer1"));
                char player[256]; snprintf(player, sizeof(player), "%s/player0", paths[connected]);
                assert(!strcmp(path, player));
                song = (song + (!strcmp(method, "Next") ? 1 : 3)) % 4;
                playing = true;
            }
            else if(!strcmp(method, "RemoveDevice")) {
                const char *device; assert(dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID));
                for(unsigned i = 0; i < 3; ++i) if(!strcmp(device, paths[i])) {
                    paired[i] = false; if(connected == (int)i) connected = -1;
                }
            }
            else assert(!"Unexpected Bluetooth method");
        }
        assert(dbus_connection_send(service, reply, NULL)); dbus_connection_flush(service);
        dbus_message_unref(reply); dbus_message_unref(message);
    }
    return NULL;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text); if(found) return found;
    }
    return NULL;
}

static lv_obj_t *label_prefix(lv_obj_t *root, const char *prefix)
{
    if(lv_obj_check_type(root, &lv_label_class) &&
       !strncmp(lv_label_get_text(root), prefix, strlen(prefix))) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label_prefix(lv_obj_get_child(root, i), prefix); if(found) return found;
    }
    return NULL;
}

static void settle(void)
{ usleep(10000); lv_tick_inc(20); lv_timer_handler(); lv_obj_update_layout(lv_screen_active()); }

static binbot_bluetooth_snapshot_t wait_ready(ui_app_t *app)
{
    binbot_bluetooth_snapshot_t snapshot;
    for(unsigned i = 0; i < 500; ++i) {
        settle(); assert(binbot_bluetooth_manager_snapshot(app->bluetooth, &snapshot));
        if(snapshot.ready && !snapshot.busy) { for(unsigned j = 0; j < 8; ++j) settle(); return snapshot; }
    }
    assert(!"Bluetooth worker timed out"); return snapshot;
}

static void click(lv_obj_t *button) { assert(button); lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); }

static lv_obj_t *button_at(lv_obj_t *root, int x, int y)
{
    lv_area_t area; lv_obj_get_coords(root, &area);
    if(lv_obj_has_flag(root, LV_OBJ_FLAG_CLICKABLE) && area.x1 == x && area.y1 == y) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = button_at(lv_obj_get_child(root, i), x, y); if(found) return found;
    }
    return NULL;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_BLUETOOTH_SNAPSHOT"); if(!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/bluetooth-%s.ppm", directory, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *p = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {p[2], p[1], p[0]}; assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file); lv_draw_buf_destroy(frame);
}

int main(void)
{
    int output[2]; assert(!pipe(output)); pid_t daemon = fork(); assert(daemon >= 0);
    if(!daemon) {
        assert(!prctl(PR_SET_PDEATHSIG, SIGTERM));
        dup2(output[1], STDOUT_FILENO); close(output[0]); close(output[1]);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address=1", NULL); _exit(127);
    }
    close(output[1]); FILE *stream = fdopen(output[0], "r"); assert(stream);
    char address[512]; assert(fgets(address, sizeof(address), stream)); address[strcspn(address, "\n")] = 0; fclose(stream);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1); dbus_threads_init_default();
    DBusError error; dbus_error_init(&error); service = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error); assert(service);
    assert(dbus_bus_request_name(service, "org.bluez", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    pthread_t thread; assert(!pthread_create(&thread, NULL, server, NULL));
    setenv("BINBOT_INITIAL_ROUTE", "bluetooth", 1);
    char images[] = "/tmp/binbot-bluetooth-XXXXXX"; assert(mkdtemp(images)); setenv("BINBOT_MODULE_IMAGE_ROOT", images, 1);
    lv_init(); lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); ui_app_t *app = ui_app_create(state); assert(app);
    binbot_bluetooth_snapshot_t current = wait_ready(app);
    assert(current.available && current.powered && current.discoverable && current.playing);
    assert(current.volume == 55 && current.volume_supported && current.playback_supported && current.device_count == 3);
    assert(!strcmp(current.title, "晴天") && !strcmp(current.artist, "周杰伦"));
    assert(label(app->current.root, "正在播放 · A2DP"));
    assert(label(app->current.root, "晴天 · 周杰伦")); snapshot(app, "playing");
    click(button_at(app->current.root, 548, 128));
    current = wait_ready(app);
    assert(!strcmp(current.title, "Lemon") && label(app->current.root, "Lemon · 米津玄师")); snapshot(app, "next");
    click(button_at(app->current.root, 432, 128));
    current = wait_ready(app); assert(!strcmp(current.title, "晴天"));
    assert(!binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_REMOVE, "/invalid", 0));
    assert(!binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_VOLUME, NULL, 101));
    click(button_at(app->current.root, 486, 124));
    current = wait_ready(app); assert(!current.playing && label(app->current.root, "已暂停 · A2DP")); snapshot(app, "paused");
    assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_PLAY, NULL, 0));
    current = wait_ready(app); assert(current.playing);
    for(unsigned i = 0; i < 2; ++i) {
        click(button_at(app->current.root, 548, 128)); current = wait_ready(app);
    }
    assert(!strcmp(current.title, titles[2]));
    char long_track[400]; snprintf(long_track, sizeof(long_track), "%s · %s", titles[2], artists[2]);
    lv_obj_t *track_label = label_prefix(app->current.root, "这是一首名称很长"); assert(track_label);
    assert(strstr(lv_label_get_text(track_label), "..."));
    assert(lv_obj_get_width(track_label) == 294 && lv_label_get_long_mode(track_label) == LV_LABEL_LONG_DOT);
    assert(lv_obj_get_height(track_label) == lv_font_get_line_height(lv_obj_get_style_text_font(track_label, 0)));
    snapshot(app, "long-track");
    click(button_at(app->current.root, 548, 128)); current = wait_ready(app);
    assert(!current.title[0] && !current.artist[0]);
    assert(label(app->current.root, "未提供曲目信息") && !label_prefix(app->current.root, "这是一首名称很长")); snapshot(app, "no-track");
    assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_VOLUME, NULL, 80));
    current = wait_ready(app); assert(current.volume == 80);
    click(lv_obj_get_parent(label(app->current.root, "断开")));
    current = wait_ready(app); assert(!current.devices[0].connected && label(app->current.root, "等待连接")); snapshot(app, "waiting");
    assert(!current.title[0] && !current.artist[0]);
    assert(!binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_NEXT, NULL, 0));
    assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_CONNECT, paths[1], 0));
    for(unsigned i = 0; i < 10; ++i) settle();
    assert(label(app->current.root, "正在连接…")); snapshot(app, "connecting");
    ui_app_navigate(app, UI_ROUTE_SETTINGS); for(unsigned i = 0; i < 5; ++i) settle();
    assert(app->route == UI_ROUTE_SETTINGS);
    click(lv_obj_get_parent(label(app->current.root, "管理")));
    for(unsigned i = 0; i < 5; ++i) settle();
    assert(app->route == UI_ROUTE_BLUETOOTH);
    current = wait_ready(app); assert(current.error[0] && !current.devices[0].connected);
    assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_CONNECT, paths[0], 0));
    current = wait_ready(app); assert(current.devices[0].connected && !current.error[0]);
    click(button_at(app->current.root, 619, 299));
    current = wait_ready(app); assert(!current.discoverable);
    click(button_at(app->current.root, 630, 33));
    current = wait_ready(app); assert(!current.powered && label(app->current.root, "蓝牙已关闭")); snapshot(app, "off");
    assert(!binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_CONNECT, paths[0], 0));
    assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_POWER, NULL, 1)); wait_ready(app);
    for(unsigned i = 0; i < 3; ++i) {
        assert(binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_REMOVE, paths[i], 0)); wait_ready(app);
    }
    assert(label(app->current.root, "还没有配对过的设备")); snapshot(app, "empty");
    ui_app_navigate(app, UI_ROUTE_SETTINGS); for(unsigned i = 0; i < 10; ++i) settle();
    assert(app->route == UI_ROUTE_SETTINGS); click(lv_obj_get_parent(label(app->current.root, "管理")));
    for(unsigned i = 0; i < 10; ++i) settle();
    assert(app->route == UI_ROUTE_BLUETOOTH);
    ui_app_set_control_center_drag_active(app, true); for(unsigned i = 0; i < 10; ++i) settle();
    ui_app_set_control_center_drag_active(app, false);
    assert(atomic_load(&calls) >= 17);
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    atomic_store(&stop, true); pthread_join(thread, NULL); dbus_connection_close(service); dbus_connection_unref(service);
    kill(daemon, SIGTERM); assert(waitpid(daemon, NULL, 0) == daemon); rmdir(images);
    puts("Bluetooth D-Bus state/actions/errors, previous/next, track metadata/empty/ellipsis, Figma states and lifecycle passed");
    return 0;
}
