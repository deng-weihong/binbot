#define _DEFAULT_SOURCE
#include "binbot/app_workspace.h"
#include "ui_internal.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static cJSON *field(cJSON *value, const char *key)
{ cJSON *item = cJSON_GetObjectItemCaseSensitive(value, key); assert(item); return item; }

static lv_obj_t *visible_label(lv_obj_t *object, const char *text)
{
    if(lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for(uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = visible_label(lv_obj_get_child(object, i), text);
        if(found) return found;
    }
    return NULL;
}

static void press_label(ui_app_t *ui, const char *text)
{
    lv_obj_t *label = visible_label(ui->current.root, text);
    assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
}
static int command(app_workspace_t *workspace, const char *name, const char *json)
{
    cJSON *payload = cJSON_Parse(json); assert(payload);
    char message[256];
    int result = app_workspace_command(workspace, name, payload,
        "12345678-1234-1234-1234-123456789abc", 42, message, sizeof(message));
    cJSON_Delete(payload); return result;
}

int main(void)
{
    char directory[] = "/tmp/binbot-workspace-XXXXXX"; assert(mkdtemp(directory));
    char path[256]; snprintf(path, sizeof(path), "%s/state.ini", directory);
    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 32 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    binbot_audio_client_t audio = {.fd = sockets[0], .role = BINBOT_AUDIO_ROLE_APP};
    app_runtime_t *runtime = app_runtime_create(path); assert(runtime);
    app_state_t *state = app_runtime_state(runtime);
    app_vocabulary_t baseline = app_state_model(state)->vocabulary; baseline.cycles_completed = 7;
    assert(app_state_set_vocabulary(state, baseline.cards, baseline.card_count, &baseline));
    ui_app_t *ui = ui_app_create(state); assert(ui);
    ui_app_set_audio_client(ui, &audio);
    app_workspace_t *workspace = app_workspace_create(runtime, ui, &audio, path); assert(workspace);

    assert(command(workspace, "todo.replace", "{\"items\":[{\"id\":\"stable-one\",\"text\":\"Read a book\",\"done\":false}]}") == 1);
    assert(command(workspace, "todo.replace", "{\"items\":[{\"id\":\"duplicate\",\"text\":\"one\",\"done\":false},{\"id\":\"duplicate\",\"text\":\"two\",\"done\":false}]}") == 0);
    assert(app_state_model(state)->todo_count == 1);
    app_todo_t native_todo = app_state_model(state)->todos[0];
    snprintf(native_todo.time, sizeof(native_todo.time), "11:30");
    snprintf(native_todo.tag, sizeof(native_todo.tag), "工作"); native_todo.tone = 0x123456;
    assert(app_state_replace_todos(state, &native_todo, 1));
    assert(command(workspace, "todo.replace", "{\"items\":[{\"id\":\"stable-one\",\"text\":\"Read a book\",\"done\":true}]}") == 1);
    assert(!strcmp(app_state_model(state)->todos[0].time, "11:30"));
    assert(!strcmp(app_state_model(state)->todos[0].tag, "工作"));
    assert(app_state_model(state)->todos[0].tone == 0x123456);

    /* A remote selection displays the requested renderer immediately from any page. */
    binbot_ambient_display_t *ambient = ui_screensaver_display(ui->screensaver);
    assert(!binbot_ambient_active(ambient));
    assert(command(workspace, "watchface.set", "{\"themeId\":\"pixel-room\",\"standbyId\":\"pixel-room\",\"idleSeconds\":60,\"shuffle\":false,\"showNow\":true}") == 1);
    assert(!strcmp(binbot_ambient_current_theme(ambient), "pixel-room"));
    assert(command(workspace, "watchface.set", "{\"themeId\":\"napping-cat\",\"standbyId\":\"napping-cat\",\"idleSeconds\":60,\"shuffle\":false,\"showNow\":true}") == 1);
    assert(!strcmp(binbot_ambient_current_theme(ambient), "napping-cat"));
    assert(command(workspace, "watchface.set", "{\"themeId\":\"missing\",\"standbyId\":\"pixel-room\",\"idleSeconds\":60,\"shuffle\":false,\"showNow\":true}") == 0);
    assert(!strcmp(app_state_model(state)->screensaver.theme_id, "napping-cat"));
    assert(!strcmp(binbot_ambient_current_theme(ambient), "napping-cat"));

    assert(command(workspace, "focus.set", "{\"action\":\"start\",\"minutes\":5}") == 1);
    assert(ui->route == UI_ROUTE_POMODORO && ui->current.root && !binbot_ambient_active(ambient));
    assert(app_state_model(state)->pomodoro.running);
    assert(app_state_model(state)->pomodoro.focus_seconds == 300);
    ui_app_go_back(ui);
    lv_tick_inc(33);
    lv_timer_handler();
    assert(ui->route == UI_ROUTE_HOME);
    assert(command(workspace, "focus.set", "{\"action\":\"pause\"}") == 1);
    assert(!app_state_model(state)->pomodoro.running);
    assert(ui->route == UI_ROUTE_HOME);
    assert(command(workspace, "watchface.set", "{\"themeId\":\"napping-cat\",\"standbyId\":\"napping-cat\",\"idleSeconds\":300,\"shuffle\":false}") == 1);
    assert(!binbot_ambient_active(ambient));

    assert(command(workspace, "learning.deck.set",
        "{\"book_id\":\"custom-book\",\"deck_id\":\"deck-one\","
        "\"plan\":{\"due\":1,\"new\":1,\"learned_today\":2,\"next_review_at\":1800000000,\"new_mask\":2},"
        "\"cards\":[{\"word\":\"persist\",\"phonetic\":\"\",\"meaning\":\"keep\",\"example\":\"Changes persist.\",\"translation\":\"\"},"
        "{\"word\":\"restore\",\"phonetic\":\"\",\"meaning\":\"recover\",\"example\":\"\",\"translation\":\"\"}]}") == 1);
    assert(app_state_model(state)->vocabulary.card_count == 2);
    assert(app_state_model(state)->vocabulary.cycles_completed == 7);
    ui_app_navigate(ui, UI_ROUTE_VOCABULARY);
    lv_tick_inc(33); lv_timer_handler();
    assert(ui->route == UI_ROUTE_VOCABULARY && ui_app_studying_vocabulary(ui));
    assert(visible_label(ui->current.root, "persist"));
    assert(!visible_label(ui->current.root, "keep"));
    assert(!visible_label(ui->current.root, "Changes persist."));
    press_label(ui, "查看释义");
    assert(visible_label(ui->current.root, "keep"));
    press_label(ui, "认识");
    assert(visible_label(ui->current.root, "restore"));
    assert(!visible_label(ui->current.root, "recover"));
    cJSON *snapshot = app_workspace_snapshot(workspace);
    cJSON *events = field(snapshot, "learning_events"); assert(cJSON_GetArraySize(events) == 1);
    char event_id[96]; snprintf(event_id, sizeof(event_id), "%s", field(cJSON_GetArrayItem(events, 0), "id")->valuestring);
    assert(!strcmp(field(cJSON_GetArrayItem(events, 0), "book_id")->valuestring, "custom-book"));
    assert(!strcmp(field(cJSON_GetArrayItem(events, 0), "grade")->valuestring, "known"));
    assert(cJSON_IsFalse(field(cJSON_GetArrayItem(events, 0), "retry")));
    cJSON_Delete(snapshot);
    ui_app_destroy(ui); app_workspace_destroy(workspace); app_runtime_destroy(runtime);

    runtime = app_runtime_create(path); assert(runtime); state = app_runtime_state(runtime);
    ui = ui_app_create(state); assert(ui); ui_app_set_audio_client(ui, &audio);
    workspace = app_workspace_create(runtime, ui, &audio, path); assert(workspace);
    assert(!strcmp(app_state_model(state)->todos[0].id, "stable-one"));
    assert(!strcmp(app_state_model(state)->screensaver.theme_id, "napping-cat"));
    assert(app_state_model(state)->vocabulary.card_count == 2);
    assert(app_state_model(state)->vocabulary.cycles_completed == 7);
    assert(app_state_model(state)->vocabulary.plan.available);
    assert(app_state_model(state)->vocabulary.plan.new_mask == 2);
    assert(app_state_model(state)->vocabulary.known_mask == 1);
    assert(!strcmp(app_state_model(state)->vocabulary.cards[1].word, "restore"));
    assert(ui_app_studying_vocabulary(ui));
    assert(command(workspace, "learning.deck.set",
        "{\"book_id\":\"custom-book\",\"deck_id\":\"replacement\",\"cards\":[]}") == 0);
    snapshot = app_workspace_snapshot(workspace);
    events = field(snapshot, "learning_events"); assert(cJSON_GetArraySize(events) == 1);
    assert(!strcmp(field(cJSON_GetArrayItem(events, 0), "id")->valuestring, event_id));
    cJSON_Delete(snapshot);
    (void)app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_AGAIN);
    char ack[160]; snprintf(ack, sizeof(ack), "{\"event_ids\":[\"%s\"]}", event_id);
    assert(command(workspace, "learning.events.ack", ack) == 1);
    assert(command(workspace, "learning.events.ack", ack) == 1);
    snapshot = app_workspace_snapshot(workspace);
    events = field(snapshot, "learning_events"); assert(cJSON_GetArraySize(events) == 1);
    assert(strcmp(field(cJSON_GetArrayItem(events, 0), "id")->valuestring, event_id));
    cJSON_Delete(snapshot);

    binbot_audio_packet_t packet;
    /* Leaving a word sends the pronunciation stop message. */
    while(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0) {}
    assert(command(workspace, "microphone.set", "{\"enabled\":false}") == 2);
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.header.type == BINBOT_AUDIO_MSG_MICROPHONE_SET);
    assert(!packet.payload.volume_set.volume);
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_MICROPHONE_RESULT, sizeof(packet.payload.volume_result), 42, 0);
    packet.payload.volume_result.success = 1; packet.payload.volume_result.volume = 0;
    app_workspace_audio(workspace, &packet);
    assert(app_state_model(state)->mic_muted);

    assert(command(workspace, "noise.set", "{\"playing\":true,\"volume\":15,\"timerMinutes\":15,\"levels\":[55,0,0,0,35,0,0,0,0]}") == 2);
    assert(recv(sockets[1], &packet, sizeof(packet), MSG_DONTWAIT) > 0);
    assert(packet.header.type == BINBOT_AUDIO_MSG_NOISE_CONTROL && packet.payload.noise.master == 15);
    packet.header.type = BINBOT_AUDIO_MSG_NOISE_RESULT; packet.payload.noise.reserved = 1;
    app_workspace_audio(workspace, &packet);
    snapshot = app_workspace_snapshot(workspace);
    assert(cJSON_IsTrue(field(field(field(snapshot, "features"), "whiteNoise"), "playing")));
    cJSON_Delete(snapshot);
    /* Long UTF-8 cards and a full todo list must stay inside the IPC envelope. */
    app_todo_t full[APP_TODO_CAPACITY] = {0};
    for(unsigned i = 0; i < APP_TODO_CAPACITY; ++i) {
        memset(full[i].id, 'a', sizeof(full[i].id)-1); full[i].id[0] = (char)('0'+i);
        memset(full[i].text, 'b', sizeof(full[i].text)-1);
    }
    assert(app_state_replace_todos(state, full, APP_TODO_CAPACITY));
    cJSON *deck = cJSON_CreateObject(), *cards = cJSON_AddArrayToObject(deck, "cards");
    cJSON_AddStringToObject(deck, "book_id", "stress"); cJSON_AddStringToObject(deck, "deck_id", "long-cards");
    for(unsigned i = 0; i < 30; ++i) {
        char word[256]; snprintf(word, sizeof(word), "w%02u", i);
        for(unsigned j = 0; j < 75; ++j) strcat(word, "词");
        cJSON *card = cJSON_CreateObject(); cJSON_AddStringToObject(card, "word", word);
        cJSON_AddStringToObject(card, "meaning", "meaning"); cJSON_AddStringToObject(card, "phonetic", "");
        cJSON_AddStringToObject(card, "example", ""); cJSON_AddStringToObject(card, "translation", "");
        cJSON_AddItemToArray(cards, card);
    }
    char message[256];
    assert(app_workspace_command(workspace, "learning.deck.set", deck, "12345678-1234-1234-1234-123456789abc", 43, message, sizeof(message)) == 1);
    cJSON_Delete(deck);
    for(unsigned i = 0; i < 30; ++i) (void)app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_KNOWN);
    snapshot = app_workspace_snapshot(workspace);
    char *encoded = cJSON_PrintUnformatted(snapshot); assert(encoded && strlen(encoded) < 14000); free(encoded);
    assert(field(field(field(snapshot, "features"), "vocabulary"), "knownCount")->valueint == 30);
    assert(field(field(field(snapshot, "features"), "vocabulary"), "cyclesCompleted")->valueint == 8);
    cJSON_Delete(snapshot);

    assert(command(workspace, "learning.deck.set",
        "{\"book_id\":\"retry-book\",\"deck_id\":\"retry\",\"cards\":[{\"word\":\"first\",\"meaning\":\"one\"},"
        "{\"word\":\"second\",\"meaning\":\"two\"}]}") == 1);
    ui_app_navigate(ui, UI_ROUTE_VOCABULARY);
    lv_tick_inc(33); lv_timer_handler();
    assert(!app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_FUZZY));
    assert(app_state_model(state)->vocabulary.current_index == 1);
    assert(!app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_KNOWN));
    assert(app_state_model(state)->vocabulary.current_index == 0);
    assert(app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_AGAIN));
    assert(app_state_model(state)->vocabulary.complete);
    assert(app_state_model(state)->vocabulary.known_mask == 2);
    assert(visible_label(ui->current.root, "本批完成"));
    assert(command(workspace, "learning.deck.set",
        "{\"book_id\":\"retry-book\",\"deck_id\":\"next\",\"cards\":[]}") == 0);
    press_label(ui, "返回");
    lv_tick_inc(33); lv_timer_handler();
    assert(!ui_app_studying_vocabulary(ui));
    assert(command(workspace, "learning.deck.set",
        "{\"book_id\":\"retry-book\",\"deck_id\":\"next\",\"cards\":[]}") == 1);
    ui_app_destroy(ui); app_workspace_destroy(workspace); app_runtime_destroy(runtime);
    close(sockets[0]); close(sockets[1]);
    unlink(path); snprintf(path, sizeof(path), "%s/state.ini.workspace.json", directory); unlink(path); rmdir(directory);
    lv_display_delete(display); lv_deinit();
    puts("workspace: durable offline learning, ACK, stable todos, focus and audio results passed");
    return 0;
}
