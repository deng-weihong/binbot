#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(unsigned milliseconds)
{
    while(milliseconds) {
        unsigned step = milliseconds > 20 ? 20 : milliseconds;
        lv_tick_inc(step); lv_timer_handler(); milliseconds -= step;
    }
    lv_obj_update_layout(lv_screen_active());
}
static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text); if(found) return found;
    }
    return NULL;
}
static void click(ui_app_t *app, const char *text)
{
    lv_obj_t *object = label(app->current.root, text); assert(object);
    while(object && !lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE)) object = lv_obj_get_parent(object);
    assert(object); lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); advance(80);
}
static binbot_audio_music_control_t receive_control(int fd)
{
    binbot_audio_packet_t packet;
    assert(recv(fd, &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.header.type == BINBOT_AUDIO_MSG_MUSIC_CONTROL);
    assert(packet.payload.music_control.reserved == BINBOT_AUDIO_MEDIA_PRONUNCIATION);
    return packet.payload.music_control;
}
static void file(const char *root, const char *accent, const char *word)
{
    char path[256]; snprintf(path, sizeof(path), "%s/%s/%s.mp3", root, accent, word);
    FILE *output = fopen(path, "wb"); assert(output);
    const unsigned char header[] = {0xff, 0xf3, 0x84, 0xc4, 0, 0, 0, 0};
    assert(fwrite(header, 1, sizeof(header), output) == sizeof(header)); fclose(output);
}
static void snapshot(ui_app_t *app)
{
    const char *path = getenv("BINBOT_VOCABULARY_SNAPSHOT"); if(!path) return;
    lv_draw_buf_t *image = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(image);
    FILE *output = fopen(path, "wb"); assert(output); fprintf(output, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const unsigned char *p = image->data + y * image->header.stride + x * 3;
        unsigned char rgb[] = {p[2], p[1], p[0]}; assert(fwrite(rgb, 1, 3, output) == 3);
    }
    fclose(output); lv_draw_buf_destroy(image);
}

int main(void)
{
    char directory[] = "/tmp/binbot-vocabulary-audio-XXXXXX"; assert(mkdtemp(directory));
    char path[256], resolved[256];
    snprintf(path, sizeof(path), "%s/us", directory); assert(!mkdir(path, 0700));
    snprintf(path, sizeof(path), "%s/uk", directory); assert(!mkdir(path, 0700));
    setenv("BINBOT_VOCABULARY_AUDIO_ROOT", directory, 1);
    for(unsigned i = 0; i < 2; ++i) {
        const char *accent = i ? "uk" : "us";
        file(directory, accent, "serene"); file(directory, accent, "focus");
        file(directory, accent, "China"); file(directory, accent, "china");
    }
    assert(app_vocabulary_audio_path("China", APP_VOCABULARY_ACCENT_US, resolved, sizeof(resolved)));
    assert(strstr(resolved, "/us/China.mp3"));
    assert(app_vocabulary_audio_path("china", APP_VOCABULARY_ACCENT_UK, resolved, sizeof(resolved)));
    assert(strstr(resolved, "/uk/china.mp3"));
    assert(app_vocabulary_audio_path("FOCUS", APP_VOCABULARY_ACCENT_US, resolved, sizeof(resolved)));
    assert(strstr(resolved, "/us/focus.mp3"));
    assert(!app_vocabulary_audio_path("../serene", APP_VOCABULARY_ACCENT_US, resolved, sizeof(resolved)));
    assert(!app_vocabulary_audio_path("serene", 99, resolved, sizeof(resolved)));
    assert(!app_vocabulary_audio_path("serene", APP_VOCABULARY_ACCENT_US, resolved, 2));
    assert(!app_vocabulary_audio_path("notcollected", APP_VOCABULARY_ACCENT_US, resolved, sizeof(resolved)));

    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    lv_init(); lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 32 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    binbot_audio_client_t audio = {.fd = sockets[0], .role = BINBOT_AUDIO_ROLE_APP};
    ui_app_set_audio_client(app, &audio);
    ui_app_navigate(app, UI_ROUTE_VOCABULARY); advance(100);
    assert(label(app->current.root, "美音") && label(app->current.root, "英音")); snapshot(app);
    app->music.active = app->music.playing = app->music.repeating = true;
    app->music.position_seconds = 40;
    click(app, "美音");
    binbot_audio_music_control_t control = receive_control(sockets[1]);
    assert(control.command == BINBOT_AUDIO_MUSIC_PLAY && strstr(control.path, "/us/serene.mp3"));
    assert(!app->music.playing && !app->music.active && app->music.position_seconds == 40);
    assert(!app_state_model(state)->vocabulary.reviewed_count);
    advance(1100);
    binbot_audio_packet_t empty;
    assert(recv(sockets[1], &empty, sizeof(empty), MSG_DONTWAIT) < 0);
    click(app, "英音"); control = receive_control(sockets[1]);
    assert(control.command == BINBOT_AUDIO_MUSIC_PLAY && strstr(control.path, "/uk/serene.mp3"));
    click(app, "serene"); control = receive_control(sockets[1]);
    assert(strstr(control.path, "/us/serene.mp3"));
    (void)app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_KNOWN); advance(80);
    control = receive_control(sockets[1]); assert(control.command == BINBOT_AUDIO_MUSIC_STOP);
    click(app, "英音"); control = receive_control(sockets[1]); assert(strstr(control.path, "/uk/focus.mp3"));
    app->voice_state = BINBOT_DIALOG_STATE_LISTENING;
    assert(!ui_app_pronounce_word(app, "focus", APP_VOCABULARY_ACCENT_US));
    assert(recv(sockets[1], &empty, sizeof(empty), MSG_DONTWAIT) < 0);
    app->voice_state = BINBOT_DIALOG_STATE_IDLE;
    assert(!ui_app_pronounce_word(app, "notcollected", APP_VOCABULARY_ACCENT_US));
    assert(recv(sockets[1], &empty, sizeof(empty), MSG_DONTWAIT) < 0);
    ui_app_navigate(app, UI_ROUTE_HOME); advance(80);
    control = receive_control(sockets[1]); assert(control.command == BINBOT_AUDIO_MUSIC_STOP);
    binbot_audio_client_close(&audio); close(sockets[1]);
    ui_app_destroy(app); app_state_destroy(state); lv_display_delete(display); lv_deinit();
    for(unsigned i = 0; i < 2; ++i) {
        const char *accent = i ? "uk" : "us";
        const char *words[] = {"serene", "focus", "China", "china"};
        for(unsigned j = 0; j < 4; ++j) { snprintf(path, sizeof(path), "%s/%s/%s.mp3", directory, accent, words[j]); unlink(path); }
        snprintf(path, sizeof(path), "%s/%s", directory, accent); rmdir(path);
    }
    rmdir(directory);
    puts("offline pronunciation lookup, both accent buttons, word tap, media isolation and page teardown verified");
    return 0;
}
