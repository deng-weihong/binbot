#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "binbot/agent/ipc.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static lv_indev_data_t pointer;

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = pointer;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *textarea(lv_obj_t *root)
{
    if(lv_obj_check_type(root, &lv_textarea_class)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = textarea(lv_obj_get_child(root, i));
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *visible_label(lv_obj_t *root, const char *text)
{
    if(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = visible_label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void settle(void)
{
    lv_tick_inc(500); lv_timer_handler(); lv_obj_update_layout(lv_screen_active());
}

static void pointer_sample(lv_indev_t *input, int x, int y, lv_indev_state_t state)
{
    pointer.point = (lv_point_t){x, y};
    pointer.state = state;
    lv_indev_read(input);
    settle();
}

static void drag(lv_indev_t *input, int x, int y, int dy)
{
    pointer_sample(input, x, y, LV_INDEV_STATE_PRESSED);
    for(int step = 1; step <= 12; ++step)
        pointer_sample(input, x, y + dy * step / 12, LV_INDEV_STATE_PRESSED);
    pointer_sample(input, x, y + dy, LV_INDEV_STATE_RELEASED);
    for(unsigned i = 0; i < 12; ++i) settle();
}

static void tap(lv_indev_t *input, int x, int y)
{
    pointer_sample(input, x, y, LV_INDEV_STATE_PRESSED);
    pointer_sample(input, x, y, LV_INDEV_STATE_RELEASED);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_ASSISTANT_SNAPSHOT"); if(!directory) return;
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
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

static uint32_t orb_signature(ui_app_t *app)
{
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    uint32_t hash = 2166136261U;
    for(unsigned y = 100; y < 420; ++y)
        for(unsigned x = 180 * 3; x < 540 * 3; ++x)
            hash = (hash ^ frame->data[y * frame->header.stride + x]) * 16777619U;
    lv_draw_buf_destroy(frame);
    return hash;
}

static void *agent_reply(void *data)
{
    int listener = *(int *)data;
    int fd = accept(listener, NULL, NULL); assert(fd >= 0);
    binbot_agent_message_type_t type;
    char request[BINBOT_AGENT_REQUEST_MAX + 1], error[256]; size_t size;
    assert(binbot_agent_ipc_read(fd, &type, request, sizeof(request), &size, 2000, error, sizeof(error)));
    assert(type == BINBOT_AGENT_MSG_REQUEST && !strcmp(request, "真实文字请求"));
    usleep(120000);
    const char *reply = "这是来自对话服务的回复。";
    assert(binbot_agent_ipc_write(fd, BINBOT_AGENT_MSG_RESPONSE, reply, strlen(reply), 2000, error, sizeof(error)));
    close(fd);
    return NULL;
}

int main(void)
{
    char directory[] = "/tmp/binbot-assistant-test-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_COMPONENT_STATE_DIR", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "assistant", 1);
    lv_init(); lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *pointer_input = lv_indev_create(); assert(pointer_input);
    lv_indev_set_type(pointer_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer_input, pointer_read);
    lv_indev_set_display(pointer_input, display);
    app_state_t *state = app_state_create(); ui_app_t *app = ui_app_create(state); assert(app);
    settle();
    assert(label(app->current.root, "暂无对话") && label(app->current.root, "服务未连接"));
    assert(visible_label(app->current.root, "轻点光球，和我聊聊吧"));
    assert(!visible_label(app->current.root, "暂无对话"));
    assert(!label(app->current.root, "今天下午有什么安排？"));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE); settle();
    assert(label(app->current.root, "在线 · 随时待命"));
    snapshot(app, "assistant-empty");
    uint32_t idle_orb = orb_signature(app);
    settle();
    assert(orb_signature(app) != idle_orb);
    snapshot(app, "assistant-empty-moving");
    tap(pointer_input, 666, 50);
    assert(visible_label(app->current.root, "暂无对话") && visible_label(app->current.root, "HISTORY · 0 条"));
    snapshot(app, "assistant-empty-history");
    uint32_t covered_orb = orb_signature(app);
    settle();
    assert(orb_signature(app) == covered_orb);
    /* Tapping the sheet does not dismiss it; tapping the backdrop does. */
    tap(pointer_input, 360, 550);
    assert(visible_label(app->current.root, "对话记录"));
    tap(pointer_input, 360, 100);
    assert(!visible_label(app->current.root, "对话记录"));

    /* Exercise the reference design's real transcript and calendar link. */
    binbot_audio_dialog_text_t reference = {.turn_id = {240}, .role = BINBOT_DIALOG_TEXT_USER, .final = 1};
    strcpy(reference.text, "今天下午有什么安排？");
    ui_app_handle_dialog_text(app, &reference);
    reference.role = BINBOT_DIALOG_TEXT_ASSISTANT;
    strcpy(reference.text, "14:30 有产品评审会，之后 16:00 提醒你喝水休息。");
    ui_app_handle_dialog_text(app, &reference); settle();
    assert(visible_label(app->current.root, "查看今日日程"));
    snapshot(app, "assistant-figma-reference");
    tap(pointer_input, 666, 50);
    snapshot(app, "assistant-figma-history");
    tap(pointer_input, 667, 190);
    tap(pointer_input, 360, 531);
    assert(app->route == UI_ROUTE_CALENDAR);
    ui_app_navigate(app, UI_ROUTE_ASSISTANT); settle();
    int audio_sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, audio_sockets));
    binbot_audio_client_t audio;
    binbot_audio_client_init(&audio, BINBOT_AUDIO_ROLE_APP, 0, 0, NULL);
    audio.fd = audio_sockets[0]; ui_app_set_audio_client(app, &audio);
    tap(pointer_input, 360, 262);
    binbot_audio_packet_t wake;
    assert(recv(audio_sockets[1], &wake, sizeof(wake), MSG_DONTWAIT) ==
           (ssize_t)(sizeof(wake.header) + sizeof(wake.payload.wake)));
    assert(wake.header.type == BINBOT_AUDIO_MSG_WAKE_REQUEST);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_LISTENING); settle();
    binbot_audio_dialog_text_t text = {.turn_id = {1}, .role = BINBOT_DIALOG_TEXT_USER};
    strcpy(text.text, "今天深圳"); ui_app_handle_dialog_text(app, &text); settle();
    assert(label(app->current.root, "今天深圳") && label(app->current.root, "正在聆听…"));
    snapshot(app, "assistant-listening-live");
    assert(ui_app_finish_voice_input(app));
    binbot_audio_packet_t end;
    assert(recv(audio_sockets[1], &end, sizeof(end), MSG_DONTWAIT) == (ssize_t)sizeof(end.header));
    assert(end.header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END && !end.header.payload_size);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_THINKING); settle();
    assert(ui_app_finish_voice_input(app));
    assert(recv(audio_sockets[1], &end, sizeof(end), MSG_DONTWAIT) == (ssize_t)sizeof(end.header));
    assert(end.header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END && !end.header.payload_size);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_SPEAKING); settle();
    tap(pointer_input, 665, 665);
    assert(recv(audio_sockets[1], &end, sizeof(end), MSG_DONTWAIT) == (ssize_t)sizeof(end.header));
    assert(end.header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END && !end.header.payload_size);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_THINKING); settle();
    strcpy(text.text, "今天深圳天气怎么样？"); text.final = 1;
    ui_app_handle_dialog_text(app, &text); settle();
    assert(!label(app->current.root, "今天深圳") && label(app->current.root, text.text));
    ui_chat_preview_t preview; ui_chat_get_preview(app, &preview);
    unsigned revision = preview.revision;
    ui_app_handle_dialog_text(app, &text); ui_chat_get_preview(app, &preview);
    assert(preview.revision == revision);
    text.final = 0; strcpy(text.text, "迟到的识别片段");
    ui_app_handle_dialog_text(app, &text); ui_chat_get_preview(app, &preview);
    assert(!strcmp(preview.question, "今天深圳天气怎么样？") && preview.revision == revision);
    text.role = BINBOT_DIALOG_TEXT_ASSISTANT; strcpy(text.text, "深圳今天");
    ui_app_handle_dialog_text(app, &text); settle();
    strcpy(text.text, "深圳今天晴天。"); text.final = 1;
    ui_app_handle_dialog_text(app, &text); settle();
    assert(!label(app->current.root, "深圳今天") && label(app->current.root, text.text));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE); settle();
    snapshot(app, "assistant-voice");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_SPEAKING); settle();
    snapshot(app, "assistant-speaking");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE); settle();
    ui_app_navigate(app, UI_ROUTE_HOME); settle();
    ui_chat_get_preview(app, &preview);
    assert(!strcmp(preview.question, "今天深圳天气怎么样？") && label(app->current.root, "深圳今天晴天。"));
    snapshot(app, "home-conversation");
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_DISCONNECTED); settle();
    assert(label(app->current.root, "服务未连接"));
    text.turn_id[0] = 2; text.role = BINBOT_DIALOG_TEXT_USER;
    strcpy(text.text, "后台收到的识别文字"); ui_app_handle_dialog_text(app, &text); settle();
    assert(app->route == UI_ROUTE_HOME && label(app->current.root, text.text));
    ui_chat_get_preview(app, &preview); assert(!preview.reply[0]);

    ui_app_navigate(app, UI_ROUTE_ASSISTANT); settle();
    for(unsigned turn = 3; turn <= 5; ++turn) {
        text.turn_id[0] = turn; text.role = BINBOT_DIALOG_TEXT_USER; text.final = 1;
        snprintf(text.text, sizeof(text.text), "第 %u 轮：请给我一些建议。", turn);
        ui_app_handle_dialog_text(app, &text); settle();
        text.role = BINBOT_DIALOG_TEXT_ASSISTANT; text.final = turn != 5;
        snprintf(text.text, sizeof(text.text), "第 %u 轮建议\n先整理今天需要完成的事情。\n给每一件事留出足够的时间。\n"
                 "工作一会儿之后站起来活动。\n喝一杯水，看看远处。\n最后回顾今天的收获。", turn);
        ui_app_handle_dialog_text(app, &text); settle();
    }
    tap(pointer_input, 666, 50);
    lv_obj_t *sheet = lv_obj_get_parent(label(app->current.root, "对话记录")); assert(sheet);
    lv_obj_t *last = label(sheet, text.text); assert(last);
    lv_obj_t *messages = lv_obj_get_parent(lv_obj_get_parent(last));
    int32_t bottom = lv_obj_get_scroll_y(messages); assert(bottom > 400);
    drag(pointer_input, 300, 320, 230);
    assert(lv_obj_get_scroll_y(messages) < bottom - 100);
    int32_t history_y = lv_obj_get_scroll_y(messages);
    strcat(text.text, "\n还有新的流式建议。\n消息继续增长。\n保持轻松的节奏。");
    ui_app_handle_dialog_text(app, &text); settle();
    assert(abs(lv_obj_get_scroll_y(messages) - history_y) <= 1);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_SPEAKING); settle();
    assert(abs(lv_obj_get_scroll_y(messages) - history_y) <= 1);
    snapshot(app, "assistant-scrolled-history");
    for(unsigned i = 0; i < 12 && lv_obj_get_scroll_bottom(messages) > 8; ++i)
        drag(pointer_input, 300, 450, -230);
    assert(lv_obj_get_scroll_bottom(messages) <= 8);
    bottom = lv_obj_get_scroll_y(messages);
    strcat(text.text, "\n在底部时自动跟随新内容。\n不会漏掉最新回复。\n现在回答结束。");
    ui_app_handle_dialog_text(app, &text); settle();
    assert(lv_obj_get_scroll_y(messages) > bottom + 20 && lv_obj_get_scroll_bottom(messages) <= 1);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_DISCONNECTED); settle();
    bottom = lv_obj_get_scroll_y(messages);
    pointer_sample(pointer_input, 300, 320, LV_INDEV_STATE_PRESSED);
    pointer_sample(pointer_input, 300, 340, LV_INDEV_STATE_PRESSED);
    history_y = lv_obj_get_scroll_y(messages);
    assert(lv_obj_is_scrolling(messages) && history_y < bottom && lv_obj_get_scroll_bottom(messages) <= 24);
    strcat(text.text, "\n拖动过程中继续收到回复。"); text.final = 1;
    ui_app_handle_dialog_text(app, &text); settle();
    assert(abs(lv_obj_get_scroll_y(messages) - history_y) <= 1);
    pointer_sample(pointer_input, 300, 450, LV_INDEV_STATE_PRESSED);
    pointer_sample(pointer_input, 300, 450, LV_INDEV_STATE_RELEASED);
    for(unsigned i = 0; i < 12; ++i) settle();
    assert(lv_obj_get_scroll_bottom(messages) > 100);

    char socket_path[108]; snprintf(socket_path, sizeof(socket_path), "%s/agent.sock", directory);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, socket_path);
    assert(!bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 1));
    setenv("BINBOT_AGENT_SOCKET", socket_path, 1);
    pthread_t thread; assert(!pthread_create(&thread, NULL, agent_reply, &listener));
    lv_obj_t *input = textarea(app->current.root); assert(input);
    tap(pointer_input, 667, 190);
    assert(!visible_label(app->current.root, "对话记录"));
    pointer_sample(pointer_input, 200, 660, LV_INDEV_STATE_PRESSED);
    pointer_sample(pointer_input, 200, 660, LV_INDEV_STATE_RELEASED);
    assert(lv_obj_get_y(lv_obj_get_parent(input)) == 377);
    snapshot(app, "assistant-keyboard");
    lv_textarea_set_text(input, "真实文字请求"); lv_obj_send_event(input, LV_EVENT_READY, NULL); settle();
    assert(label(app->current.root, "思考中…") && label(app->current.root, "真实文字请求"));
    assert(lv_obj_get_y(lv_obj_get_parent(input)) == 633 && lv_obj_get_scroll_bottom(messages) <= 1);
    ui_app_navigate(app, UI_ROUTE_HOME); settle();
    assert(!pthread_join(thread, NULL));
    for(unsigned i = 0; i < 100; ++i) {
        usleep(10000); settle(); ui_chat_get_preview(app, &preview);
        if(preview.reply[0]) break;
    }
    assert(!strcmp(preview.reply, "这是来自对话服务的回复。"));
    assert(label(app->current.root, preview.reply));
    ui_app_navigate(app, UI_ROUTE_ASSISTANT); settle();
    assert(label(app->current.root, "真实文字请求") && label(app->current.root, preview.reply));
    snapshot(app, "assistant-text");
    tap(pointer_input, 666, 50);
    assert(visible_label(app->current.root, "真实文字请求") && visible_label(app->current.root, preview.reply));
    snapshot(app, "assistant-text-history");
    tap(pointer_input, 667, 190);
    assert(!visible_label(app->current.root, "对话记录"));

    /* A reply taller than the screen remains readable from its first line to
     * its last, both inline and through the explicit full-reply button. */
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE);
    text.turn_id[0] = 80; text.role = BINBOT_DIALOG_TEXT_USER; text.final = 1;
    strcpy(text.text, "请详细介绍今天的专注计时安排。");
    ui_app_handle_dialog_text(app, &text);
    text.role = BINBOT_DIALOG_TEXT_ASSISTANT; text.final = 0;
    size_t used = 0;
    for(unsigned line = 1; line <= 34; ++line) {
        int written = snprintf(text.text + used, sizeof(text.text) - used,
                               "第 %u 条：专注工作之后，记得起身活动并喝一杯水。\n", line);
        assert(written > 0 && (size_t)written < sizeof(text.text) - used);
        used += (size_t)written;
    }
    ui_app_handle_dialog_text(app, &text); settle();
    lv_obj_t *inline_reply = visible_label(app->current.root, text.text); assert(inline_reply);
    lv_obj_t *reply_view = lv_obj_get_parent(inline_reply);
    assert(lv_label_get_long_mode(inline_reply) == LV_LABEL_LONG_WRAP);
    assert(lv_obj_get_height(inline_reply) > lv_obj_get_height(reply_view) + 500);
    lv_obj_t *more = visible_label(app->current.root, "上滑阅读 · 展开全文"); assert(more);
    lv_obj_t *more_button = lv_obj_get_parent(more);
    lv_area_t more_area, summary_area;
    lv_obj_get_coords(more_button, &more_area);
    lv_obj_get_coords(lv_obj_get_parent(more_button), &summary_area);
    assert(more_area.y1 >= summary_area.y1 && more_area.y2 <= summary_area.y2);
    assert(more_area.y2 < 578); /* Never covered by the quick questions. */
    assert(visible_label(app->current.root, "查看专注计时"));
    snapshot(app, "assistant-long-reply-main");
    /* Playback updates move the view without changing the complete answer.
     * Late/invalid cues cannot replace text or restart an interrupted turn. */
    binbot_audio_dialog_text_t speech = text;
    char *cursor = strstr(speech.text, "第 18 条"); assert(cursor); *cursor = '\0';
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_SPEAKING); settle();
    ui_chat_get_preview(app, &preview); revision = preview.revision;
    ui_app_handle_dialog_playback(app, &speech); settle();
    int32_t spoken_y = lv_obj_get_scroll_y(reply_view); assert(spoken_y > 250);
    assert(visible_label(app->current.root, text.text));
    assert(visible_label(app->current.root, "随播滚动 · 展开全文"));
    ui_chat_get_preview(app, &preview); assert(preview.revision == revision);
    snapshot(app, "assistant-speech-follow");
    speech.turn_id[0] = 79;
    ui_app_handle_dialog_playback(app, &speech); settle();
    assert(lv_obj_get_scroll_y(reply_view) == spoken_y);
    speech.turn_id[0] = text.turn_id[0];
    strcpy(speech.text, "不属于本轮回复的内容");
    ui_app_handle_dialog_playback(app, &speech); settle();
    assert(lv_obj_get_scroll_y(reply_view) == spoken_y);
    lv_area_t speech_area; lv_obj_get_coords(reply_view, &speech_area);
    pointer_sample(pointer_input, 300, speech_area.y1 + 10, LV_INDEV_STATE_PRESSED);
    pointer_sample(pointer_input, 300, speech_area.y1 + 36, LV_INDEV_STATE_PRESSED);
    pointer_sample(pointer_input, 300, speech_area.y1 + 36, LV_INDEV_STATE_RELEASED);
    int32_t manual_y = lv_obj_get_scroll_y(reply_view); assert(manual_y < spoken_y);
    speech = text; cursor = strstr(speech.text, "第 26 条"); assert(cursor); *cursor = '\0';
    ui_app_handle_dialog_playback(app, &speech); settle();
    assert(lv_obj_get_scroll_y(reply_view) == manual_y);
    assert(visible_label(app->current.root, "已暂停跟随 · 展开全文"));
    for(unsigned i = 0; i < 11; ++i) settle();
    assert(lv_obj_get_scroll_y(reply_view) > spoken_y);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE); settle();
    lv_obj_scroll_to_y(reply_view, 0, LV_ANIM_OFF);
    ui_app_handle_dialog_playback(app, &speech); settle();
    assert(lv_obj_get_scroll_y(reply_view) == 0);
    assert(!visible_label(app->current.root, "随播滚动 · 展开全文"));
    lv_area_t view_area; lv_obj_get_coords(reply_view, &view_area);
    drag(pointer_input, 300, view_area.y2 - 8, -28);
    int32_t inline_y = lv_obj_get_scroll_y(reply_view); assert(inline_y > 0);
    strcat(text.text, "接下来继续补充具体建议。\n");
    ui_app_handle_dialog_text(app, &text); settle();
    assert(abs(lv_obj_get_scroll_y(reply_view) - inline_y) <= 1);
    snapshot(app, "assistant-long-reply-inline-scrolled");
    tap(pointer_input, (more_area.x1 + more_area.x2) / 2, (more_area.y1 + more_area.y2) / 2);
    assert(visible_label(app->current.root, "对话记录"));
    sheet = lv_obj_get_parent(label(app->current.root, "对话记录"));
    last = label(sheet, text.text); assert(last);
    messages = lv_obj_get_parent(lv_obj_get_parent(last));
    lv_area_t bubble_area, messages_area;
    lv_obj_get_coords(lv_obj_get_parent(last), &bubble_area);
    lv_obj_get_coords(messages, &messages_area);
    assert(abs(bubble_area.y1 - messages_area.y1) <= 1);
    assert(lv_obj_get_scroll_bottom(messages) > 200);
    history_y = lv_obj_get_scroll_y(messages);
    snapshot(app, "assistant-long-reply-expanded-start");
    strcat(text.text, "这是完整回复的最后一行，阅读到这里就结束了。"); text.final = 1;
    ui_app_handle_dialog_text(app, &text); settle();
    assert(abs(lv_obj_get_scroll_y(messages) - history_y) <= 1);
    for(unsigned i = 0; i < 24 && lv_obj_get_scroll_bottom(messages) > 1; ++i)
        drag(pointer_input, 300, 490, -220);
    assert(lv_obj_get_scroll_bottom(messages) <= 1);
    last = label(sheet, text.text); assert(last);
    assert(lv_label_get_long_mode(last) == LV_LABEL_LONG_WRAP);
    lv_area_t last_area; lv_obj_get_coords(last, &last_area);
    assert(last_area.y2 <= messages_area.y2 && last_area.y2 >= messages_area.y1);
    snapshot(app, "assistant-long-reply-expanded-end");
    tap(pointer_input, 667, 190);
    text.turn_id[0] = 81; text.role = BINBOT_DIALOG_TEXT_USER;
    strcpy(text.text, "你好"); ui_app_handle_dialog_text(app, &text);
    text.role = BINBOT_DIALOG_TEXT_ASSISTANT; strcpy(text.text, "你好，有什么可以帮你？");
    ui_app_handle_dialog_text(app, &text); settle();
    assert(!visible_label(app->current.root, "上滑阅读 · 展开全文"));
    inline_reply = visible_label(app->current.root, text.text); assert(inline_reply);
    assert(lv_obj_get_scroll_y(lv_obj_get_parent(inline_reply)) == 0);
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    binbot_audio_client_close(&audio); close(audio_sockets[1]);
    close(listener); assert(!unlink(socket_path)); assert(!rmdir(directory));
    puts("assistant orb wake, history drawer, real IPC, touch scrolling, stream retention, bottom following, full long replies, transcripts and home preview passed");
    return 0;
}
