#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static lv_indev_data_t pointer;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) { (void)indev; *data = pointer; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(unsigned ms)
{
    while(ms) { unsigned step = ms > 20 ? 20 : ms; lv_tick_inc(step); lv_timer_handler(); ms -= step; }
    lv_obj_update_layout(lv_screen_active());
}
static void click(int x, int y)
{
    pointer.point = (lv_point_t){x, y}; pointer.state = LV_INDEV_STATE_PRESSED; advance(40);
    pointer.state = LV_INDEV_STATE_RELEASED; advance(80);
}
static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text); if(found) return found;
    }
    return NULL;
}
static binbot_audio_packet_t received(int fd)
{
    binbot_audio_packet_t packet = {0};
    assert(recv(fd, &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.header.type == BINBOT_AUDIO_MSG_NOISE_CONTROL);
    return packet;
}
static void snapshot(ui_app_t *app, const char *name)
{
    const char *out = getenv("BINBOT_HOME_SNAPSHOT"); if(!out) return;
    char path[512]; snprintf(path, sizeof(path), "%s/noise-%s-native.ppm", out, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *p = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {p[2], p[1], p[0]}; assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file); lv_draw_buf_destroy(frame);
}
static void assets(lv_obj_t *root)
{
    const void *src = lv_obj_check_type(root, &lv_image_class) ? lv_image_get_src(root) :
                      lv_obj_get_style_bg_image_src(root, 0);
    if(src) { lv_image_header_t header; assert(lv_image_decoder_get_info(src, &header) == LV_RESULT_OK); assert(header.w && header.h); }
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) assets(lv_obj_get_child(root, i));
}
int main(void)
{
    setenv("BINBOT_INITIAL_ROUTE", "noise", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    binbot_audio_client_t client = {.fd = sockets[0], .role = BINBOT_AUDIO_ROLE_APP};
    ui_app_set_audio_client(app, &client);
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);
    advance(500); assets(app->current.root);
    assert(label(app->current.root, "雨声 + 咖啡馆")); assert(!app->noise.playing);
    snapshot(app, "paused");
    click(445, 120); binbot_audio_packet_t packet = received(sockets[1]);
    assert(packet.payload.noise.playing && packet.payload.noise.levels[1] == 60 && packet.payload.noise.levels[5] == 25);
    assert(label(app->current.root, "海浪 + 风声")); snapshot(app, "sleep");
    click(124, 655); packet = received(sockets[1]); assert(packet.payload.noise.timer_seconds == 1800);
    assert(label(app->current.root, "30:00 后停止")); snapshot(app, "timer");
    click(76, 588); packet = received(sockets[1]); assert(!packet.payload.noise.playing);
    snapshot(app, "timer-paused");
    click(180, 588); packet = received(sockets[1]); assert(!packet.payload.noise.playing && packet.payload.noise.master < 70);
    assert(app->noise.levels[1] == 60 && app->noise.levels[5] == 25);
    click(525, 304); packet = received(sockets[1]);
    assert(packet.payload.noise.levels[1] >= 5 && packet.payload.noise.levels[1] != 60);
    assert(!app->noise.playing && app->noise.levels[1] > 0);
    click(650, 120); packet = received(sockets[1]);
    assert(packet.payload.noise.levels[0] == 60 && packet.payload.noise.levels[2] == 30 && packet.payload.noise.levels[6] == 45);
    snapshot(app, "rainy");
    click(547, 120); (void)received(sockets[1]); snapshot(app, "relax");
    binbot_audio_noise_state_t all = app->noise;
    memset(all.levels, 100, sizeof(all.levels)); ui_app_handle_noise_state(app, &all);
    snapshot(app, "all"); assets(app->current.root);
    click(54, 50); assert(app->route == UI_ROUTE_HOME && app->noise.playing);
    ui_app_navigate(app, UI_ROUTE_NOISE); advance(80); assert(app->noise.playing && app->noise.levels[8] == 100);
    all.playing = 0; all.timer_seconds = all.remaining_seconds = 0;
    ui_app_handle_noise_state(app, &all); assert(label(app->current.root, "不限时"));
    for(unsigned i = 0; i < 9; ++i) { click(350 + (i % 3) * 137, 230 + (i / 3) * 184); (void)received(sockets[1]); }
    assert(!app->noise.playing && label(app->current.root, "安静一下"));
    snapshot(app, "empty");
    click(76, 588); assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) < 0);
    ui_app_navigate(app, UI_ROUTE_APPS); advance(80);
    assert(label(app->current.root, "白噪音")); snapshot(app, "more");
    click(260, 285); assert(app->route == UI_ROUTE_NOISE);
    ui_app_navigate(app, UI_ROUTE_POMODORO); advance(80); click(350, 630); assert(app->route == UI_ROUTE_NOISE);
    binbot_audio_client_close(&client); close(sockets[1]);
    click(445, 120); assert(!app->noise.playing);
    for(unsigned i = 0; i < 15; ++i) {
        ui_app_navigate(app, UI_ROUTE_HOME); advance(80);
        ui_app_navigate(app, UI_ROUTE_NOISE); advance(80);
    }
    ui_app_destroy(app); app_state_destroy(state); lv_indev_delete(input); lv_display_delete(display); lv_deinit();
    puts("white noise controls, independent volumes, timer states, routing, local assets and repeated teardown verified");
    return 0;
}
