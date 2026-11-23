#include "../ui_internal.h"
#include "assistant_orb.h"
#include "binbot/agent/ipc.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "misc/lv_text_private.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { CHAT_HISTORY_CAPACITY = 16 };
typedef struct {
    bool from_user;
    bool final;
    uint8_t turn_id[16];
    char text[BINBOT_DIALOG_TEXT_MAX + 1];
    char time[32];
} chat_message_t;

struct ui_chat_session {
    pthread_mutex_t mutex;
    unsigned references;
    unsigned revision;
    bool busy;
    bool failed;
    size_t count;
    unsigned playback_revision;
    uint32_t playback_chars;
    uint8_t playback_turn_id[16];
    chat_message_t messages[CHAT_HISTORY_CAPACITY];
};

typedef struct {
    ui_chat_session_t *session;
    char socket_path[108];
    char text[BINBOT_AGENT_REQUEST_MAX + 1];
} chat_job_t;

typedef struct {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *dot;
    lv_obj_t *messages;
    lv_obj_t *input;
    lv_obj_t *send;
    lv_obj_t *mic;
    lv_obj_t *mic_icon;
    lv_obj_t *keyboard;
    lv_obj_t *input_bar;
    lv_obj_t *summary, *question, *reply_view, *reply, *thinking, *read_more, *read_more_text;
    lv_obj_t *action, *action_icon, *action_text, *action_arrow;
    lv_obj_t *prompts, *history, *history_count;
    lv_obj_t *orb_hit, *mic_wave, *mic_dots;
    ui_assistant_orb_t *orb;
    ui_route_t action_route;
    char background[512];
    lv_timer_t *timer;
    unsigned revision;
    int previous_state;
    int latest_reply_y;
    unsigned playback_revision;
    uint32_t manual_scroll_tick;
    bool manual_scroll;
    bool reveal_message;
    bool history_open, keyboard_open, paused;
} assistant_context_t;

static chat_message_t *append_message(ui_chat_session_t *session, bool user, const char *text)
{
    if(session->count == CHAT_HISTORY_CAPACITY) {
        memmove(session->messages, session->messages + 2,
                  (CHAT_HISTORY_CAPACITY - 2) * sizeof(session->messages[0]));
        session->count -= 2;
    }
    chat_message_t *message = &session->messages[session->count++];
    memset(message, 0, sizeof(*message));
    message->from_user = user;
    message->final = true;
    snprintf(message->text, sizeof(message->text), "%s", text);
    time_t now = time(NULL);
    struct tm local;
    if(localtime_r(&now, &local)) strftime(message->time, sizeof(message->time), "%H:%M", &local);
    session->revision++;
    return message;
}

void ui_chat_session_destroy(ui_chat_session_t *session)
{
    if(!session) return;
    pthread_mutex_lock(&session->mutex);
    bool release = --session->references == 0;
    pthread_mutex_unlock(&session->mutex);
    if(release) {
        pthread_mutex_destroy(&session->mutex);
        free(session);
    }
}

static ui_chat_session_t *session_create(void)
{
    ui_chat_session_t *session = calloc(1, sizeof(*session));
    if(!session) return NULL;
    if(pthread_mutex_init(&session->mutex, NULL)) { free(session); return NULL; }
    session->references = 1;
    session->revision = 1;
    return session;
}

void ui_app_handle_dialog_text(ui_app_t *app, const binbot_audio_dialog_text_t *text)
{
    if(!app || !binbot_audio_dialog_text_is_valid(text) || !text->text[0]) return;
    if(!app->chat) app->chat = session_create();
    ui_chat_session_t *session = app->chat;
    if(!session) return;
    bool user = text->role == BINBOT_DIALOG_TEXT_USER;
    pthread_mutex_lock(&session->mutex);
    session->failed = false;
    chat_message_t *message = NULL;
    for(size_t i = 0; i < session->count; ++i) {
        if(session->messages[i].from_user == user &&
           !memcmp(session->messages[i].turn_id, text->turn_id, sizeof(text->turn_id))) {
            message = &session->messages[i];
            break;
        }
    }
    if(!message) {
        message = append_message(session, user, text->text);
        memcpy(message->turn_id, text->turn_id, sizeof(message->turn_id));
        message->final = text->final;
    }
    else if(!message->final && (text->final || strcmp(message->text, text->text))) {
        snprintf(message->text, sizeof(message->text), "%s", text->text);
        message->final = text->final;
        session->revision++;
    }
    pthread_mutex_unlock(&session->mutex);
    if(app->route == UI_ROUTE_ASSISTANT && !app->route_pending && !app->content_frozen)
        ui_assistant_screen_sync(app->current.context);
}

static int chat_state(ui_app_t *app)
{
    return app->chat && app->chat->busy ? BINBOT_DIALOG_STATE_THINKING :
           app->chat && app->chat->failed ? BINBOT_DIALOG_STATE_ERROR : app->voice_state;
}

void ui_chat_reset_playback(ui_app_t *app)
{
    if(!app || !app->chat) return;
    ui_chat_session_t *session = app->chat;
    pthread_mutex_lock(&session->mutex);
    session->playback_chars = 0;
    memset(session->playback_turn_id, 0, sizeof(session->playback_turn_id));
    session->playback_revision++;
    pthread_mutex_unlock(&session->mutex);
}

void ui_app_handle_dialog_playback(ui_app_t *app, const binbot_audio_dialog_text_t *text)
{
    if(!app || app->voice_state != BINBOT_DIALOG_STATE_SPEAKING || !app->chat ||
       !binbot_audio_dialog_text_is_valid(text) || text->role != BINBOT_DIALOG_TEXT_ASSISTANT ||
       !text->text[0]) return;
    ui_chat_session_t *session = app->chat;
    pthread_mutex_lock(&session->mutex);
    chat_message_t *reply = session->count ? &session->messages[session->count - 1] : NULL;
    size_t length = strlen(text->text);
    if(reply && !reply->from_user && !memcmp(reply->turn_id, text->turn_id, 16) &&
       length <= strlen(reply->text) && !memcmp(reply->text, text->text, length)) {
        uint32_t chars = lv_text_get_encoded_length(text->text);
        if(memcmp(session->playback_turn_id, text->turn_id, 16)) {
            memcpy(session->playback_turn_id, text->turn_id, 16);
            session->playback_chars = 0;
        }
        if(chars > session->playback_chars) {
            session->playback_chars = chars;
            session->playback_revision++;
        }
    }
    pthread_mutex_unlock(&session->mutex);
    if(app->route == UI_ROUTE_ASSISTANT && !app->route_pending && !app->content_frozen)
        ui_assistant_screen_sync(app->current.context);
}

const char *ui_chat_status_text(int state)
{
    switch(state) {
        case BINBOT_DIALOG_STATE_IDLE: return "在线 · 随时待命";
        case BINBOT_DIALOG_STATE_LISTENING: return "正在聆听…";
        case BINBOT_DIALOG_STATE_THINKING: return "思考中…";
        case BINBOT_DIALOG_STATE_SPEAKING: return "正在回复…";
        case BINBOT_DIALOG_STATE_CANCELLING: return "正在结束…";
        case BINBOT_DIALOG_STATE_ERROR: return "服务异常";
        default: return "服务未连接";
    }
}

void ui_chat_get_preview(ui_app_t *app, ui_chat_preview_t *preview)
{
    memset(preview, 0, sizeof(*preview));
    preview->state = app->voice_state;
    ui_chat_session_t *session = app->chat;
    if(!session) return;
    pthread_mutex_lock(&session->mutex);
    preview->revision = session->revision;
    preview->state = chat_state(app);
    for(size_t i = 0; i < session->count; ++i) {
        chat_message_t *message = &session->messages[i];
        if(message->from_user) {
            snprintf(preview->question, sizeof(preview->question), "%s", message->text);
            preview->reply[0] = '\0';
        }
        else snprintf(preview->reply, sizeof(preview->reply), "%s", message->text);
    }
    pthread_mutex_unlock(&session->mutex);
}

/* The worker retains only the session. It never touches an LVGL object, so a
 * response can safely arrive after navigation or UI shutdown. */
static void *chat_worker(void *user_data)
{
    chat_job_t *job = user_data;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    char reply[BINBOT_AGENT_RESPONSE_MAX + 1], error[256];
    bool ok = binbot_agent_request(job->socket_path, job->text, reply, sizeof(reply),
                                     90000, error, sizeof(error));
    ui_chat_session_t *session = job->session;
    pthread_mutex_lock(&session->mutex);
    append_message(session, false, ok ? reply : "暂时无法连接对话服务，请稍后重试。");
    session->busy = false;
    session->failed = !ok;
    pthread_mutex_unlock(&session->mutex);
    if(!ok) fprintf(stderr, "ui: chat request failed: %s\n", error);
    free(job);
    ui_chat_session_destroy(session);
    return NULL;
}

static bool ask(assistant_context_t *context, const char *text)
{
    if(!text || !text[0]) return false;
    size_t length = strlen(text);
    if(length > BINBOT_AGENT_REQUEST_MAX) {
        ui_app_show_toast(context->app, "消息过长");
        return false;
    }
    ui_chat_session_t *session = context->app->chat;
    pthread_mutex_lock(&session->mutex);
    if(session->busy || (context->app->voice_state >= BINBOT_DIALOG_STATE_LISTENING &&
                        context->app->voice_state <= BINBOT_DIALOG_STATE_SPEAKING)) {
        pthread_mutex_unlock(&session->mutex);
        ui_app_show_toast(context->app, "正在等待回复");
        return false;
    }
    chat_job_t *job = calloc(1, sizeof(*job));
    if(!job) { pthread_mutex_unlock(&session->mutex); return false; }
    const char *socket_path = getenv("BINBOT_AGENT_SOCKET");
    if(!socket_path || !socket_path[0]) socket_path = "/tmp/binbot_agent.sock";
    if(strlen(socket_path) >= sizeof(job->socket_path)) {
        free(job);
        pthread_mutex_unlock(&session->mutex);
        return false;
    }
    job->session = session;
    snprintf(job->socket_path, sizeof(job->socket_path), "%s", socket_path);
    snprintf(job->text, sizeof(job->text), "%s", text);
    session->busy = true;
    session->failed = false;
    session->references++;
    pthread_attr_t attributes;
    pthread_t thread;
    int status = pthread_attr_init(&attributes);
    if(!status) {
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        status = pthread_create(&thread, &attributes, chat_worker, job);
        pthread_attr_destroy(&attributes);
    }
    if(status) {
        session->references--;
        session->busy = false;
        free(job);
        pthread_mutex_unlock(&session->mutex);
        ui_app_show_toast(context->app, "暂时无法发送消息");
        return false;
    }
    append_message(session, true, text);
    pthread_mutex_unlock(&session->mutex);
    context->reveal_message = true;
    return true;
}

static int add_bubble(assistant_context_t *context, const chat_message_t *message, int y)
{
    lv_obj_t *label = ui_ref_label(context->messages, message->text, 17,
                                     UI_FONT_REGULAR, 0xffffff, 0, 0);
    lv_obj_set_style_text_opa(label, message->from_user ? 217 : 230, 0);
    lv_obj_set_style_text_line_space(label, 23 - ui_ref_font(17, UI_FONT_REGULAR)->line_height, 0);
    lv_obj_update_layout(label);
    int width = lv_obj_get_width(label);
    if(width > 467) {
        width = 467;
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(label);
    }
    int height = lv_obj_get_height(label) + 26;
    int x = message->from_user ? 662 - width - 36 : 0;
    lv_obj_t *bubble = ui_box(context->messages, x, y, width + 36, height,
                                message->from_user ? 0x27272e : 0x1b1b22, 22);
    if(!message->from_user) {
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_opa(bubble, 20, 0);
    }
    lv_obj_set_parent(label, bubble);
    lv_obj_set_pos(label, message->from_user ? 18 : 17, message->from_user ? 13 : 12);
    lv_obj_t *corner = ui_box(bubble, message->from_user ? width + 14 : 0,
                                height - 22, 22, 22, message->from_user ? 0x27272e : 0x1b1b22, 6);
    lv_obj_move_background(corner);
    if(message->from_user && message->time[0]) {
        lv_obj_t *time = ui_ref_label(context->messages, message->time, 15,
                                        UI_FONT_REGULAR, 0xffffff, x, y + height + 4);
        lv_obj_set_style_text_opa(time, 77, 0);
        lv_obj_set_width(time, width + 29);
        lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_RIGHT, 0);
        height += 27;
    }
    return y + height + 12;
}

static void hidden(lv_obj_t *object, bool hide)
{
    if(hide) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void motion_sync(assistant_context_t *context)
{
    ui_assistant_orb_set_paused(context->orb, context->paused ||
                               context->history_open || context->keyboard_open);
}

static void action_sync(assistant_context_t *context, const char *question, bool visible, int y)
{
    const char *text = NULL, *icon = NULL;
    if(visible && question) {
        if(strstr(question, "计时") || strstr(question, "专注") || strstr(question, "番茄")) {
            text = "查看专注计时"; icon = "focus"; context->action_route = UI_ROUTE_POMODORO;
        } else if(strstr(question, "提醒")) {
            text = "查看提醒"; icon = "bell"; context->action_route = UI_ROUTE_TODO;
        } else if(strstr(question, "白噪音")) {
            text = "打开白噪音"; icon = "music"; context->action_route = UI_ROUTE_NOISE;
        } else if(strstr(question, "音乐")) {
            text = "打开音乐"; icon = "music"; context->action_route = UI_ROUTE_MUSIC;
        } else if(strstr(question, "日程") || strstr(question, "会议") || strstr(question, "安排")) {
            text = "查看今日日程"; icon = "cal"; context->action_route = UI_ROUTE_CALENDAR;
        }
    }
    hidden(context->action, !text);
    if(!text) return;
    /* These links navigate to real features; don't claim that a reminder or
     * timer was created merely because its keyword appeared in a request. */
    lv_label_set_text(context->action_text, text);
    ui_ref_icon_set_name(context->action_icon, icon);
    lv_obj_update_layout(context->action_text);
    int width = lv_obj_get_width(context->action_text);
    lv_obj_set_width(context->action, width + 80);
    lv_obj_set_pos(context->action, (608 - width - 80) / 2, y);
    lv_obj_set_pos(context->action_arrow, width + 49, 12);
}

static int summary_text(lv_obj_t *label, const char *text, int y, int max_height)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_text(label, text);
    lv_obj_set_y(label, y);
    lv_obj_update_layout(label);
    int height = lv_obj_get_height(label);
    if(height > max_height) {
        height = max_height;
        lv_obj_set_height(label, height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    return height;
}

static int reply_text(assistant_context_t *context, const char *text, int y, int max_height)
{
    int32_t scroll_y = lv_obj_get_scroll_y(context->reply_view);
    const char *previous = lv_label_get_text(context->reply);
    /* Keep the reader's position as a streamed reply grows, but start a new
     * answer at its first line. Never replace the stored text with dots. */
    if(strncmp(text, previous, strlen(previous)) != 0) scroll_y = 0;
    lv_obj_set_y(context->reply_view, y);
    lv_obj_set_height(context->reply_view, max_height);
    lv_label_set_long_mode(context->reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(context->reply, LV_SIZE_CONTENT);
    lv_label_set_text(context->reply, text);
    lv_obj_update_layout(context->reply);
    int height = lv_obj_get_height(context->reply);
    lv_obj_set_style_text_align(context->reply, height > max_height ?
                               LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    if(height > max_height) height = max_height;
    lv_obj_set_height(context->reply_view, height);
    lv_obj_update_layout(context->reply_view);
    lv_obj_scroll_to_y(context->reply_view, scroll_y, LV_ANIM_OFF);
    return height;
}

static void pause_playback_follow(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    context->manual_scroll = true;
    context->manual_scroll_tick = lv_tick_get();
}

static void playback_follow(assistant_context_t *context, ui_chat_session_t *session, int state)
{
    bool active = state == BINBOT_DIALOG_STATE_SPEAKING && session->playback_chars &&
        session->count && !session->messages[session->count - 1].from_user &&
        !memcmp(session->messages[session->count - 1].turn_id, session->playback_turn_id, 16);
    bool manual = context->manual_scroll && lv_tick_elaps(context->manual_scroll_tick) < 5000;
    const char *caption = active ? manual ? "已暂停跟随 · 展开全文" : "随播滚动 · 展开全文" :
                                                  "上滑阅读 · 展开全文";
    if(strcmp(lv_label_get_text(context->read_more_text), caption))
        lv_label_set_text(context->read_more_text, caption);
    if(!active || manual || context->history_open || context->keyboard_open || context->paused ||
       lv_obj_is_scrolling(context->reply_view)) return;
    uint32_t chars = lv_text_get_encoded_length(lv_label_get_text(context->reply));
    if(!chars) return;
    uint32_t position = session->playback_chars < chars ? session->playback_chars - 1 : chars - 1;
    lv_point_t point;
    lv_label_get_letter_pos(context->reply, position, &point);
    lv_obj_scroll_to_y(context->reply_view, point.y, LV_ANIM_OFF);
}

static void summary_sync(assistant_context_t *context, ui_chat_session_t *session, int state)
{
    const chat_message_t *question = NULL, *reply = NULL;
    for(size_t i = 0; i < session->count; ++i) {
        if(session->messages[i].from_user) { question = &session->messages[i]; reply = NULL; }
        else reply = &session->messages[i];
    }
    lv_label_set_text(context->question, question ? question->text : "");
    hidden(context->question, !question);
    hidden(context->reply, false);
    hidden(context->thinking, state != BINBOT_DIALOG_STATE_THINKING);
    int action_y = 0;
    bool long_reply = false;
    if(state == BINBOT_DIALOG_STATE_LISTENING) {
        ui_resources_set_font(context->reply, ui_ref_font(22, UI_FONT_MEDIUM), 0);
        lv_obj_set_style_text_opa(context->reply, 204, 0);
        reply_text(context, "我在听，请说…", 24, 58);
        hidden(context->question, !question || (session->count && !session->messages[session->count - 1].from_user));
        if(question) summary_text(context->question, question->text, 66, 23);
    } else if(state == BINBOT_DIALOG_STATE_THINKING) {
        hidden(context->question, true);
        ui_resources_set_font(context->reply, ui_ref_font(20, UI_FONT_MEDIUM), 0);
        lv_obj_set_style_text_opa(context->reply, 204, 0);
        int height = reply_text(context, question ? question->text : "思考中…", 12, 56);
        lv_obj_set_y(context->thinking, 12 + height + 12);
    } else {
        ui_resources_set_font(context->reply, ui_ref_font(21, UI_FONT_MEDIUM), 0);
        lv_obj_set_style_text_opa(context->reply, 230, 0);
        if(question) summary_text(context->question, question->text, 0, 23);
        int y = question ? 31 : 0;
        int height = reply_text(context, reply ? reply->text : "轻点光球，和我聊聊吧", y, 58);
        long_reply = reply && lv_obj_get_height(context->reply) > height;
        action_y = y + height + 14;
    }
    action_sync(context, question ? question->text : NULL,
                reply && state != BINBOT_DIALOG_STATE_LISTENING && state != BINBOT_DIALOG_STATE_THINKING,
                action_y);
    hidden(context->read_more, !long_reply);
    if(long_reply) {
        int x = (608 - 200) / 2;
        if(!lv_obj_has_flag(context->action, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_update_layout(context->action);
            int width = lv_obj_get_width(context->action);
            int start = (608 - width - 12 - 200) / 2;
            lv_obj_set_x(context->action, start);
            x = start + width + 12;
        }
        lv_obj_set_pos(context->read_more, x, action_y);
    }
    bool taller = long_reply || (!lv_obj_has_flag(context->action, LV_OBJ_FLAG_HIDDEN) &&
                                 action_y + 40 > 128);
    lv_obj_set_y(context->summary, taller ? 422 : 438);
    lv_obj_set_height(context->summary, taller ? 144 : 128);
    char count[64];
    snprintf(count, sizeof(count), "HISTORY · %zu 条", session->count);
    lv_label_set_text(context->history_count, count);
}

void ui_assistant_screen_sync(void *user_data)
{
    assistant_context_t *context = user_data;
    if(!context) return;
    ui_chat_session_t *session = context->app->chat;
    pthread_mutex_lock(&session->mutex);
    int state = chat_state(context->app);
    bool state_changed = context->previous_state != state;
    if(context->revision != session->revision || state_changed) {
        int32_t scroll_y = lv_obj_get_scroll_y(context->messages);
        bool follow_latest = context->reveal_message || !context->revision ||
            (!lv_obj_is_scrolling(context->messages) && lv_obj_get_scroll_bottom(context->messages) <= 24);
        lv_obj_clean(context->messages);
        int y = 0;
        context->latest_reply_y = 0;
        for(size_t i = 0; i < session->count; ++i) {
            if(!session->messages[i].from_user) context->latest_reply_y = y;
            y = add_bubble(context, &session->messages[i], y);
        }
        if(!session->count) {
            lv_obj_t *empty = ui_ref_label(context->messages, "暂无对话", 15,
                                             UI_FONT_REGULAR, 0x6b6b70, 0, 180);
            lv_obj_set_width(empty, 662);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        }
        summary_sync(context, session, state);
        lv_obj_update_layout(context->messages);
        /* Cleaning resets the offset; streamed replies must not pull the user
         * away from history or interrupt a touch drag. */
        lv_obj_scroll_to_y(context->messages, follow_latest ? LV_COORD_MAX : scroll_y, LV_ANIM_OFF);
        context->reveal_message = false;
        context->revision = session->revision;
        context->previous_state = state;
    }
    if(context->playback_revision != session->playback_revision) {
        if(!session->playback_chars) context->manual_scroll = false;
        context->playback_revision = session->playback_revision;
    }
    playback_follow(context, session, state);
    pthread_mutex_unlock(&session->mutex);
    if(!state_changed) return;
    lv_label_set_text(context->status, ui_chat_status_text(state));
    lv_obj_update_layout(context->status);
    int status_width = lv_obj_get_width(context->status);
    lv_obj_set_x(context->status, 623 - status_width);
    lv_obj_set_x(context->dot, 608 - status_width);
    uint32_t dot = state == BINBOT_DIALOG_STATE_IDLE ? 0x6fd6a4 :
        state == BINBOT_DIALOG_STATE_LISTENING ? 0x7cc8ff :
        state == BINBOT_DIALOG_STATE_THINKING ? 0xa99bff :
        state == BINBOT_DIALOG_STATE_SPEAKING ? 0xe9a2e0 : 0x73737a;
    lv_obj_set_style_bg_color(context->dot, lv_color_hex(dot), 0);
    unsigned orb_state = state == BINBOT_DIALOG_STATE_LISTENING ? 1 :
        state == BINBOT_DIALOG_STATE_THINKING ? 2 : state == BINBOT_DIALOG_STATE_SPEAKING ? 3 : 0;
    ui_assistant_orb_set_state(context->orb, orb_state);
    bool voice_active = context->app->voice_state >= BINBOT_DIALOG_STATE_LISTENING &&
        context->app->voice_state <= BINBOT_DIALOG_STATE_SPEAKING;
    ui_ref_icon_set_name(context->mic_icon, voice_active ? "stop" : "mic");
    hidden(context->mic_icon, state == BINBOT_DIALOG_STATE_LISTENING || state == BINBOT_DIALOG_STATE_THINKING);
    hidden(context->mic_wave, state != BINBOT_DIALOG_STATE_LISTENING);
    hidden(context->mic_dots, state != BINBOT_DIALOG_STATE_THINKING);
    bool disabled = state == BINBOT_DIALOG_STATE_CANCELLING ||
        (state == BINBOT_DIALOG_STATE_THINKING && !voice_active);
    if(disabled) {
        lv_obj_add_state(context->mic, LV_STATE_DISABLED);
        lv_obj_add_state(context->orb_hit, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(context->mic, LV_STATE_DISABLED);
        lv_obj_remove_state(context->orb_hit, LV_STATE_DISABLED);
    }
}

static void poll_chat(lv_timer_t *timer)
{
    assistant_context_t *context = lv_timer_get_user_data(timer);
    if(context->app->content_frozen) return;
    ui_assistant_screen_sync(context);
    if(context->history_open) return;
    unsigned phase = lv_tick_get() / 150;
    for(unsigned i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_opa(lv_obj_get_child(context->thinking, i), (phase / 2) % 3 == i ? 153 : 38, 0);
        lv_obj_set_style_bg_opa(lv_obj_get_child(context->mic_dots, i), (phase / 2) % 3 == i ? 255 : 64, 0);
    }
    static const int heights[] = {7, 13, 18, 11};
    for(unsigned i = 0; i < 4; ++i) {
        lv_obj_t *bar = lv_obj_get_child(context->mic_wave, i);
        int height = heights[(i + phase) % 4];
        lv_obj_set_height(bar, height);
        lv_obj_set_y(bar, (18 - height) / 2);
    }
}

static void keyboard_show(assistant_context_t *context, bool show)
{
    context->keyboard_open = show;
    hidden(context->keyboard, !show);
    hidden(context->summary, show);
    hidden(context->prompts, show);
    lv_obj_set_y(context->input_bar, show ? 377 : 633);
    lv_keyboard_set_textarea(context->keyboard, show ? context->input : NULL);
    if(show) {
        lv_obj_move_foreground(context->input_bar);
        lv_obj_move_foreground(context->keyboard);
    }
    motion_sync(context);
}

static void send_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    if(ask(context, lv_textarea_get_text(context->input))) {
        lv_textarea_set_text(context->input, "");
        keyboard_show(context, false);
        ui_assistant_screen_sync(context);
    }
}

static void quick_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    const char *text = lv_obj_get_user_data(lv_event_get_current_target(event));
    if(ask(context, text)) ui_assistant_screen_sync(context);
}

static void talk_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    if(context->app->voice_state >= BINBOT_DIALOG_STATE_LISTENING &&
       context->app->voice_state <= BINBOT_DIALOG_STATE_SPEAKING)
        (void)ui_app_finish_voice_input(context->app);
    else (void)ui_app_request_wake(context->app);
}

static void input_changed(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    bool nonempty = lv_textarea_get_text(context->input)[0];
    if(nonempty) lv_obj_remove_flag(context->send, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(context->send, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(context->input, nonempty ? 530 : 587);
}

static void input_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    keyboard_show(context, true);
}

static void keyboard_event(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    if(lv_event_get_code(event) == LV_EVENT_READY) send_clicked(event);
    if(lv_event_get_code(event) == LV_EVENT_CANCEL ||
       (lv_event_get_code(event) == LV_EVENT_READY && !lv_textarea_get_text(context->input)[0])) {
        keyboard_show(context, false);
    }
}

static void assistant_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    (void)model; (void)changes;
    ui_assistant_screen_sync(user_data);
}

static void assistant_paused(void *user_data, bool paused)
{
    assistant_context_t *context = user_data;
    context->paused = paused;
    motion_sync(context);
    if(paused) lv_timer_pause(context->timer);
    else { lv_timer_resume(context->timer); ui_assistant_screen_sync(context); }
}

static void assistant_destroy(void *user_data)
{
    assistant_context_t *context = user_data;
    lv_timer_delete(context->timer);
    ui_assistant_orb_destroy(context->orb);
    lv_obj_set_style_bg_image_src(context->root, NULL, 0);
    lv_image_cache_drop(context->background);
    free(context);
}

static lv_obj_t *white_label(lv_obj_t *parent, const char *text, unsigned size,
                              ui_font_weight_t weight, unsigned opacity, int x, int y)
{
    lv_obj_t *label = ui_ref_label(parent, text, size, weight, 0xffffff, x, y);
    lv_obj_set_style_text_opa(label, opacity, 0);
    return label;
}

static lv_obj_t *glass_button(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *button = ui_button(parent, x, y, width, height, 0xffffff, height / 2);
    lv_obj_set_style_bg_opa(button, 13, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(button, 20, 0);
    return button;
}

static void back_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    ui_app_go_back(context->app);
}

static void action_clicked(lv_event_t *event)
{
    assistant_context_t *context = lv_event_get_user_data(event);
    ui_app_navigate(context->app, context->action_route);
}

static void sheet_y(void *object, int32_t value) { lv_obj_set_y(object, value); }

static void history_open(assistant_context_t *context, bool latest_reply)
{
    keyboard_show(context, false);
    context->history_open = true;
    hidden(context->history, false);
    lv_obj_move_foreground(context->history);
    lv_obj_scroll_to_y(context->messages, latest_reply ? context->latest_reply_y : LV_COORD_MAX, LV_ANIM_OFF);
    motion_sync(context);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, lv_obj_get_child(context->history, 0));
    lv_anim_set_exec_cb(&animation, sheet_y);
    lv_anim_set_values(&animation, 720, 140);
    lv_anim_set_duration(&animation, 300);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static void history_show(lv_event_t *event)
{
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    history_open(lv_event_get_user_data(event), false);
}

static void full_reply_show(lv_event_t *event)
{
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    history_open(lv_event_get_user_data(event), true);
}

static void history_close(lv_event_t *event)
{
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    assistant_context_t *context = lv_event_get_user_data(event);
    context->history_open = false;
    hidden(context->history, true);
    motion_sync(context);
}

ui_screen_t ui_assistant_screen_create(ui_app_t *app)
{
    if(!app->chat) app->chat = session_create();
    if(!app->chat) return (ui_screen_t){0};
    assistant_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->previous_state = -1;
    lv_obj_t *root = ui_box(app->page_layer, 0, 0, 720, 720, 0x08080e, 0);
    context->root = root;
    const char *assets = getenv("BINBOT_ASSET_ROOT");
    if(!assets || !assets[0]) assets = "/oem/assets";
    snprintf(context->background, sizeof(context->background), "%s%s/reference/assistant/background.bin",
             assets[0] == 'A' && assets[1] == ':' ? "" : "A:", assets);
    lv_obj_set_style_bg_image_src(root, context->background, 0);
    context->orb = ui_assistant_orb_create(root);
    lv_obj_t *handle = ui_box(root, 317, 7, 86, 5, 0xffffff, 3);
    lv_obj_set_style_bg_opa(handle, 38, 0);
    context->orb_hit = ui_box(root, 240, 142, 240, 240, 0, 120);
    lv_obj_set_style_bg_opa(context->orb_hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->orb_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->orb_hit, talk_clicked, LV_EVENT_CLICKED, context);
    lv_obj_t *back = glass_button(root, 29, 25, 50, 50);
    lv_obj_t *icon = ui_ref_icon(back, "back", 24, 0xffffff);
    lv_obj_set_style_opa(icon, 204, 0); lv_obj_center(icon);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, context);
    white_label(root, "AI COMPANION", 15, UI_FONT_REGULAR, 89, 93, 27);
    white_label(root, "AI 对话", 22, UI_FONT_SEMIBOLD, 230, 93, 48);
    context->dot = ui_box(root, 507, 47, 7, 7, 0x6fd6a4, 4);
    context->status = white_label(root, "", 15, UI_FONT_REGULAR, 128, 522, 38);
    lv_obj_t *history = glass_button(root, 641, 25, 50, 50);
    icon = ui_ref_icon(history, "note", 21, 0xffffff);
    lv_obj_set_style_opa(icon, 179, 0); lv_obj_center(icon);
    lv_obj_add_event_cb(history, history_show, LV_EVENT_CLICKED, context);

    context->summary = ui_box(root, 56, 438, 608, 128, 0, 0);
    lv_obj_set_style_bg_opa(context->summary, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->summary, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->summary, full_reply_show, LV_EVENT_CLICKED, context);
    context->question = white_label(context->summary, "", 15, UI_FONT_REGULAR, 89, 0, 0);
    lv_obj_set_width(context->question, 608);
    lv_obj_set_style_text_align(context->question, LV_TEXT_ALIGN_CENTER, 0);
    context->reply_view = ui_box(context->summary, 0, 31, 608, 58, 0, 0);
    lv_obj_set_style_bg_opa(context->reply_view, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->reply_view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(context->reply_view, LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(context->reply_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->reply_view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(context->reply_view, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(context->reply_view, lv_color_hex(0xa99bff), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(context->reply_view, 153, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(context->reply_view, pause_playback_follow, LV_EVENT_PRESSED, context);
    lv_obj_add_event_cb(context->reply_view, pause_playback_follow, LV_EVENT_PRESSING, context);
    lv_obj_add_event_cb(context->reply_view, pause_playback_follow, LV_EVENT_RELEASED, context);
    context->reply = white_label(context->reply_view, "", 21, UI_FONT_MEDIUM, 230, 4, 0);
    lv_obj_set_width(context->reply, 600);
    lv_obj_set_style_text_line_space(context->reply, 29 - ui_ref_font(21, UI_FONT_MEDIUM)->line_height, 0);
    lv_obj_set_style_text_align(context->reply, LV_TEXT_ALIGN_CENTER, 0);
    context->thinking = ui_box(context->summary, 290, 61, 28, 6, 0, 0);
    lv_obj_set_style_bg_opa(context->thinking, LV_OPA_TRANSP, 0);
    for(unsigned i = 0; i < 3; ++i) ui_box(context->thinking, i * 11, 0, 6, 6, 0xffffff, 3);
    context->action = glass_button(context->summary, 200, 75, 200, 40);
    context->action_icon = ui_ref_icon(context->action, "cal", 17, 0xffffff);
    lv_obj_set_style_opa(context->action_icon, 179, 0);
    lv_obj_set_pos(context->action_icon, 15, 11);
    context->action_text = white_label(context->action, "", 15, UI_FONT_REGULAR, 179, 40, 8);
    context->action_arrow = ui_ref_icon(context->action, "fwd", 15, 0xffffff);
    lv_obj_set_style_opa(context->action_arrow, 89, 0);
    lv_obj_add_event_cb(context->action, action_clicked, LV_EVENT_CLICKED, context);
    context->read_more = glass_button(context->summary, 204, 103, 200, 40);
    context->read_more_text = ui_centered_label(context->read_more, "上滑阅读 · 展开全文",
                                                ui_ref_font(13, UI_FONT_REGULAR), 0xc4b8ff);
    lv_obj_set_style_text_opa(context->read_more_text, 230, 0);
    lv_obj_add_event_cb(context->read_more, full_reply_show, LV_EVENT_CLICKED, context);
    hidden(context->read_more, true);

    context->prompts = ui_box(root, 0, 578, 720, 40, 0, 0);
    lv_obj_set_style_bg_opa(context->prompts, LV_OPA_TRANSP, 0);
    static const char *const prompts[] = {"今天天气怎么样", "提醒我 4 点喝水", "播放白噪音", "开始专注计时"};
    int widths[4], total = 24;
    for(unsigned i = 0; i < 4; ++i) {
        lv_point_t size;
        lv_text_get_size(&size, prompts[i], ui_ref_font(15, UI_FONT_REGULAR), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        widths[i] = size.x + 32; total += widths[i];
    }
    int x = (720 - total) / 2;
    for(size_t i = 0; i < 4; ++i) {
        lv_obj_t *button = glass_button(context->prompts, x, 0, widths[i], 40);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_t *label = ui_centered_label(button, prompts[i], ui_ref_font(15, UI_FONT_REGULAR), 0xffffff);
        lv_obj_set_style_text_opa(label, 140, 0);
        lv_obj_set_user_data(button, (void *)prompts[i]);
        lv_obj_add_event_cb(button, quick_clicked, LV_EVENT_CLICKED, context);
        x += widths[i] + 8;
    }
    context->input_bar = ui_box(root, 22, 633, 676, 65, 0xffffff, 33);
    lv_obj_set_style_bg_opa(context->input_bar, 13, 0);
    lv_obj_set_style_border_width(context->input_bar, 1, 0);
    lv_obj_set_style_border_color(context->input_bar, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(context->input_bar, 20, 0);
    context->input = lv_textarea_create(context->input_bar);
    lv_obj_remove_style_all(context->input);
    lv_obj_set_pos(context->input, 23, 6);
    lv_obj_set_size(context->input, 587, 50);
    lv_textarea_set_one_line(context->input, true);
    lv_textarea_set_max_length(context->input, 512);
    lv_textarea_set_placeholder_text(context->input, "输入消息，或轻点光球说话…");
    ui_resources_set_font(context->input, ui_ref_font(17, UI_FONT_REGULAR), 0);
    lv_obj_set_style_text_color(context->input, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_opa(context->input, 230, 0);
    lv_obj_set_style_text_color(context->input, lv_color_hex(0xffffff), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_opa(context->input, 77, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_pad_top(context->input, 13, 0);
    lv_obj_add_event_cb(context->input, input_clicked, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(context->input, input_changed, LV_EVENT_VALUE_CHANGED, context);
    lv_obj_add_event_cb(context->input, send_clicked, LV_EVENT_READY, context);
    context->send = ui_button(context->input_bar, 561, 6, 50, 50, 0xffffff, 25);
    lv_obj_set_style_bg_opa(context->send, 26, 0);
    icon = ui_ref_icon(context->send, "send", 23, 0xffffff);
    lv_obj_set_style_opa(icon, 204, 0);
    lv_obj_center(icon);
    lv_obj_add_event_cb(context->send, send_clicked, LV_EVENT_CLICKED, context);
    lv_obj_add_flag(context->send, LV_OBJ_FLAG_HIDDEN);
    context->mic = ui_button(context->input_bar, 617, 6, 50, 50, 0xffffff, 25);
    context->mic_icon = ui_ref_icon(context->mic, "mic", 21, UI_REF_INK);
    lv_obj_center(context->mic_icon);
    context->mic_wave = ui_box(context->mic, 14, 16, 21, 18, 0, 0);
    lv_obj_set_style_bg_opa(context->mic_wave, LV_OPA_TRANSP, 0);
    for(unsigned i = 0; i < 4; ++i) ui_box(context->mic_wave, i * 6, 0, 3, 18, UI_REF_INK, 2);
    context->mic_dots = ui_box(context->mic, 13, 22, 23, 5, 0, 0);
    lv_obj_set_style_bg_opa(context->mic_dots, LV_OPA_TRANSP, 0);
    for(unsigned i = 0; i < 3; ++i) ui_box(context->mic_dots, i * 9, 0, 5, 5, UI_REF_INK, 3);
    lv_obj_add_event_cb(context->mic, talk_clicked, LV_EVENT_CLICKED, context);
    context->keyboard = lv_keyboard_create(root);
    lv_obj_align(context->keyboard, LV_ALIGN_TOP_LEFT, 22, 450);
    lv_obj_set_size(context->keyboard, 676, 248);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0x121219), 0);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0x27272e), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0x41414b), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(context->keyboard, lv_color_hex(0xe5e5e8), LV_PART_ITEMS);
    lv_obj_add_flag(context->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(context->keyboard, keyboard_event, LV_EVENT_READY, context);
    lv_obj_add_event_cb(context->keyboard, keyboard_event, LV_EVENT_CANCEL, context);

    context->history = ui_box(root, 0, 0, 720, 720, 0, 0);
    lv_obj_set_style_bg_opa(context->history, 128, 0);
    lv_obj_add_flag(context->history, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(context->history, history_close, LV_EVENT_CLICKED, context);
    lv_obj_t *sheet = ui_box(context->history, 0, 140, 720, 611, 0x121219, 31);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_side(sheet, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(sheet, 20, 0);
    context->history_count = white_label(sheet, "HISTORY · 0 条", 15, UI_FONT_REGULAR, 89, 29, 22);
    white_label(sheet, "对话记录", 22, UI_FONT_SEMIBOLD, 230, 29, 46);
    lv_obj_t *close = glass_button(sheet, 643, 26, 48, 48);
    icon = ui_ref_icon(close, "x", 22, 0xffffff);
    lv_obj_set_style_opa(icon, 179, 0); lv_obj_center(icon);
    lv_obj_add_event_cb(close, history_close, LV_EVENT_CLICKED, context);
    context->messages = ui_box(sheet, 29, 92, 662, 464, 0, 0);
    lv_obj_set_style_bg_opa(context->messages, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->messages, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(context->messages, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->messages, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(context->messages, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(context->messages, lv_color_hex(0xa99bff), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(context->messages, 153, LV_PART_SCROLLBAR);
    context->timer = lv_timer_create(poll_chat, 150, context);
    return (ui_screen_t){root, context, assistant_update, assistant_destroy, assistant_paused};
}
