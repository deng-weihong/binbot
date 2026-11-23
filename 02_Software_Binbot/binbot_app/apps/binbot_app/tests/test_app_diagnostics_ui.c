#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static lv_point_t position;
static lv_indev_state_t pressed;
static lv_indev_t *pointer;

static lv_obj_t *find_label(lv_obj_t *root, const char *value)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, (int)i), value);
        if(found) return found;
    }
    return NULL;
}

static void settle(void)
{
    lv_tick_inc(100); lv_timer_handler(); lv_obj_update_layout(lv_screen_active());
}

static void click_label(ui_app_t *app, const char *value)
{
    lv_obj_t *label = find_label(app->current.root, value); assert(label);
    lv_obj_t *button = label;
    while(button && !lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE)) button = lv_obj_get_parent(button);
    assert(button); lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); settle();
}

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input; data->point = position; data->state = pressed;
}

static void point(int x, int y, bool down)
{
    position = (lv_point_t){x, y}; pressed = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_tick_inc(20); lv_indev_read(pointer); lv_obj_update_layout(lv_screen_active());
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_DIAGNOSTICS_SNAPSHOT_DIR");
    if(!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for(unsigned x = 0; x < 720; ++x) {
            uint8_t pixel[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            assert(fwrite(pixel, 1, 3, file) == 3);
        }
    }
    assert(!fclose(file)); lv_draw_buf_destroy(frame);
}

static binbot_audio_packet_t receive_type(int fd, unsigned type)
{
    binbot_audio_packet_t packet;
    for(unsigned i = 0; i < 64; ++i) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        assert(poll(&descriptor, 1, 2000) == 1);
        ssize_t size = recv(fd, &packet, sizeof(packet), 0);
        assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
        if(packet.header.type == type) return packet;
    }
    abort();
}

static void connected_audio(ui_app_t *app, const char *path)
{
    int server = socket(AF_UNIX, SOCK_SEQPACKET, 0); assert(server >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    assert(!bind(server, (struct sockaddr *)&address, sizeof(address)) && !listen(server, 1));
    struct pollfd descriptor = {.fd = server, .events = POLLIN};
    assert(poll(&descriptor, 1, 2000) == 1);
    int peer = accept(server, NULL, NULL); assert(peer >= 0);
    receive_type(peer, BINBOT_AUDIO_MSG_HELLO);
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PCM_STEREO,
                             sizeof(packet.payload.pcm_stereo), 0, 0);
    for(unsigned i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
        packet.payload.pcm_stereo.samples[i * 2] = 1000;
        packet.payload.pcm_stereo.samples[i * 2 + 1] = 2000;
    }
    assert(send(peer, &packet, binbot_audio_packet_size(&packet), MSG_NOSIGNAL) > 0);
    usleep(30000); settle();
    lv_obj_t *record = find_label(app->current.root, "录音 5 秒"); assert(record);
    assert(!lv_obj_has_state(lv_obj_get_parent(record), LV_STATE_DISABLED));
    snapshot(app, "microphone-live");
    click_label(app, "录音 5 秒");
    assert(find_label(app->current.root, "录音中"));
    for(unsigned i = 0; i < 250; ++i) {
        packet.header.sequence = i + 1;
        assert(send(peer, &packet, binbot_audio_packet_size(&packet), MSG_NOSIGNAL) > 0);
    }
    usleep(30000); settle();
    assert(find_label(app->current.root, "重新录音"));
    snapshot(app, "microphone-recorded");
    click_label(app, "右声道麦克风");
    lv_obj_t *play = find_label(app->current.root, "播放右声道"); assert(play);
    assert(!lv_obj_has_state(lv_obj_get_parent(play), LV_STATE_DISABLED));
    click_label(app, "播放右声道");
    receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN);
    packet = receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_FRAME);
    for(unsigned i = 0; i < packet.payload.playback_frame.sample_count; ++i)
        assert(packet.payload.playback_frame.samples[i] == 2000);
    usleep(20000); settle(); assert(find_label(app->current.root, "停止"));
    snapshot(app, "microphone-playback");
    uint8_t volume = app_state_model(app->state)->volume;
    app_state_set_volume(app->state, 0); settle();
    assert(find_label(app->current.root, "扬声器已静音"));
    app_state_set_volume(app->state, volume); settle();
    click_label(app, "停止"); receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_CANCEL);
    click_label(app, "扬声器");
    app_state_set_volume(app->state, 0); settle();
    assert(find_label(app->current.root, "扬声器已静音"));
    app_state_set_volume(app->state, volume); settle();
    click_label(app, "右声道麦克风");
    close(peer); close(server); assert(!unlink(path));
    usleep(20000); settle();
}

int main(void)
{
    char directory[] = "/tmp/binbot-diag-ui-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "about", 1);
    char socket_path[108]; snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", directory);
    setenv("BINBOT_AUDIO_SOCKET", socket_path, 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    pointer = lv_indev_create(); lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app); settle();
    for(unsigned i = 0; i < 3; ++i) click_label(app, "系统版本");
    assert(app->route == UI_ROUTE_ABOUT);
    lv_tick_inc(1500);
    for(unsigned i = 0; i < 4; ++i) click_label(app, "系统版本");
    assert(app->route == UI_ROUTE_ABOUT);
    click_label(app, "系统版本");
    assert(app->route == UI_ROUTE_DIAGNOSTICS && find_label(app->current.root, "功能测试"));
    usleep(20000); settle();
    lv_obj_t *record = find_label(app->current.root, "录音 5 秒"); assert(record);
    assert(lv_obj_has_state(lv_obj_get_parent(record), LV_STATE_DISABLED));
    assert(find_label(app->current.root, "音频服务未连接"));
    snapshot(app, "microphone-left");
    connected_audio(app, socket_path);
    click_label(app, "右声道麦克风");
    assert(find_label(app->current.root, "播放右声道"));
    snapshot(app, "microphone-right");
    ui_app_handle_wake_detected(app, 1); ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_LISTENING);
    settle(); assert(app->route == UI_ROUTE_DIAGNOSTICS && !app->route_pending);
    assert(!ui_app_request_wake(app));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_ERROR);
    assert(!find_label(app->root, "语音服务暂时不可用"));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE);
    lv_tick_inc(61000); settle(); assert(!ui_screensaver_active(app->screensaver));
    click_label(app, "扬声器");
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(app->current.root, "1 kHz 测试音")), LV_STATE_DISABLED));
    snapshot(app, "speaker");
    click_label(app, "屏幕"); snapshot(app, "display");
    click_label(app, "开始屏幕测试");
    const char *steps[] = {"红 · 1/6 · 轻触下一项", "绿 · 2/6 · 轻触下一项", "蓝 · 3/6 · 轻触下一项",
                           "白 · 4/6 · 轻触下一项", "黑 · 5/6 · 轻触下一项", "灰阶 · 6/6 · 轻触下一项"};
    for(unsigned i = 0; i < 6; ++i) {
        assert(find_label(app->current.root, steps[i]));
        char name[32]; snprintf(name, sizeof(name), "color-%u", i); snapshot(app, name);
        point(360, 400, true); point(360, 400, false); settle();
    }
    assert(find_label(app->current.root, "滑动填满所有格子 · 0/64"));
    for(unsigned row = 0; row < 8; ++row) {
        int y = row ? (int)row * 90 + 45 : 75;
        point(45, y, true); point(675, y, true); point(675, y, false); settle();
    }
    assert(find_label(app->current.root, "滑动填满所有格子 · 64/64"));
    assert(find_label(app->current.root, "完成")); snapshot(app, "touch-complete");
    point(537, 38, true); point(537, 38, false); settle();
    assert(!find_label(app->current.root, "完成") && find_label(app->current.root, "64/64 格"));
    lv_obj_send_event(lv_obj_get_child(app->current.root, 1), LV_EVENT_CLICKED, NULL); settle();
    assert(app->route == UI_ROUTE_ABOUT);
    ui_app_destroy(app); app_state_destroy(state); lv_indev_delete(pointer); lv_deinit();
    assert(!rmdir(directory));
    puts("Hidden entry timeout, offline controls, channel tabs, six colors, 64-cell drag and cleanup passed");
    return 0;
}
