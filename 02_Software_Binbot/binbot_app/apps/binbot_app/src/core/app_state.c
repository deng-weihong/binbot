#include "binbot/app_state.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

enum {
    APP_LISTENER_CAPACITY = 8,
};

static const app_vocabulary_card_t vocabulary_cards[APP_VOCABULARY_CAPACITY] = {
    {"serene", "/səˈriːn/", "宁静的，安详的",
     "The room felt serene after the rain stopped.", "雨停后，房间显得格外安静。"},
    {"focus", "/ˈfoʊkəs/", "专注；集中",
     "A quiet desk helps me focus on one task.", "安静的书桌能帮助我专注于一件事。"},
    {"deliberate", "/dɪˈlɪbərət/", "深思熟虑的；故意的",
     "She took a deliberate pause before answering.", "她在回答前有意停顿了一下。"},
    {"resilient", "/rɪˈzɪliənt/", "有韧性的；能复原的",
     "Small habits make a resilient routine.", "小习惯能形成有韧性的日常节奏。"},
    {"curious", "/ˈkjʊriəs/", "好奇的",
     "The curious child asked how the clock worked.", "好奇的孩子问钟表是如何工作的。"},
    {"vivid", "/ˈvɪvɪd/", "生动的；鲜明的",
     "The book left a vivid picture in my mind.", "这本书在我脑中留下了生动的画面。"},
    {"gentle", "/ˈdʒentəl/", "温和的；轻柔的",
     "A gentle reminder is often enough.", "温和的提醒通常就足够了。"},
    {"insight", "/ˈɪnsaɪt/", "洞察；见解",
     "The review gave us useful insight.", "这次复盘给了我们有用的见解。"},
    {"routine", "/ruːˈtiːn/", "日常；惯例",
     "A simple morning routine keeps me steady.", "简单的晨间惯例让我保持稳定。"},
    {"balance", "/ˈbæləns/", "平衡",
     "Good work needs a balance between effort and rest.", "好的工作需要在努力和休息之间取得平衡。"},
    {"mindful", "/ˈmaɪndfəl/", "留心的；专注当下的",
     "Try to be mindful of your breathing.", "试着留意自己的呼吸。"},
    {"restore", "/rɪˈstɔːr/", "恢复；使复原",
     "A short walk can restore your energy.", "短暂散步可以恢复你的精力。"},
    {"adapt", "/əˈdæpt/", "适应；调整",
     "We can adapt the plan when conditions change.", "情况变化时我们可以调整计划。"},
    {"clarify", "/ˈklærəfaɪ/", "澄清；说明",
     "Please clarify the last step for me.", "请为我说明最后一步。"},
    {"sustain", "/səˈsteɪn/", "维持；支撑",
     "Tiny breaks sustain attention during long work.", "长时间工作时，小休息能维持注意力。"},
    {"subtle", "/ˈsʌtəl/", "细微的；不明显的",
     "There was a subtle change in her voice.", "她的声音发生了细微的变化。"},
    {"grateful", "/ˈɡreɪtfəl/", "感激的",
     "I am grateful for the quiet support.", "我很感激这份安静的支持。"},
    {"steady", "/ˈstedi/", "稳定的；稳步的",
     "Steady progress matters more than speed.", "稳定的进步比速度更重要。"},
    {"reflect", "/rɪˈflekt/", "反思；映照",
     "Take a moment to reflect on the day.", "花一点时间回顾这一天。"},
    {"accomplish", "/əˈkɑːmplɪʃ/", "完成；实现",
     "You can accomplish more with a clear next step.", "明确下一步，你就能完成更多事情。"},
    {"explore", "/ɪkˈsplɔːr/", "探索",
     "We used the weekend to explore a new path.", "我们利用周末探索了一条新路线。"},
    {"patient", "/ˈpeɪʃənt/", "耐心的",
     "Be patient while the new habit takes shape.", "新习惯形成时要保持耐心。"},
    {"inspire", "/ɪnˈspaɪər/", "鼓舞；启发",
     "Her small act can inspire the whole team.", "她的小小行动可以鼓舞整个团队。"},
    {"notice", "/ˈnoʊtɪs/", "注意到；留意",
     "Notice how your shoulders relax.", "留意你的肩膀是如何放松的。"},
    {"progress", "/ˈprɑːɡres/", "进步；进展",
     "Every review is a small step of progress.", "每次复习都是一点小小的进步。"},
    {"courage", "/ˈkɜːrɪdʒ/", "勇气",
     "It takes courage to begin again.", "重新开始需要勇气。"},
    {"arrange", "/əˈreɪndʒ/", "安排；整理",
     "Arrange the tasks by importance first.", "先按重要程度安排任务。"},
    {"release", "/rɪˈliːs/", "释放；发布",
     "Release the tension in your hands.", "放松你手上的紧张感。"},
    {"absorb", "/əbˈzɔːrb/", "吸收；理解",
     "A short review helps absorb the idea.", "简短复习有助于理解这个想法。"},
    {"thrive", "/θraɪv/", "茁壮成长；兴旺",
     "Good routines help creative work thrive.", "良好的习惯能让创作茁壮成长。"},
};

static uint32_t vocabulary_full_mask(size_t card_count)
{
    if(card_count == 0) return 0;
    if(card_count >= 32) return UINT32_MAX;
    return (1U << card_count) - 1U;
}

static size_t vocabulary_next_card(const app_vocabulary_t *vocabulary, size_t current_index)
{
    if(!vocabulary || vocabulary->card_count == 0) return 0;
    /* Finish presenting new cards before repeating any missed in this batch. */
    uint32_t skip = vocabulary->attempted_mask == vocabulary_full_mask(vocabulary->card_count)
                  ? vocabulary->finished_mask : vocabulary->attempted_mask;
    for(size_t offset = 1; offset <= vocabulary->card_count; ++offset) {
        size_t index = (current_index + offset) % vocabulary->card_count;
        if((skip & (1U << index)) == 0) return index;
    }
    return current_index;
}

typedef struct {
    app_state_listener_t callback;
    void *user_data;
} listener_slot_t;

struct app_state {
    app_model_t model;
    listener_slot_t listeners[APP_LISTENER_CAPACITY];
    uint32_t last_clock_second;
    uint32_t pomodoro_last_ms;
    uint32_t computer_wake_started_ms;
    app_vocabulary_review_fn review_handler;
    void *review_user_data;
};

static void notify(app_state_t *state, uint32_t changes)
{
    for(size_t i = 0; i < APP_LISTENER_CAPACITY; ++i) {
        if(state->listeners[i].callback) {
            state->listeners[i].callback(changes, &state->model, state->listeners[i].user_data);
        }
    }
}

static void init_alarm(app_alarm_t *alarm, const char *time, const char *label,
                       const char *repeat, bool enabled)
{
    snprintf(alarm->time, sizeof(alarm->time), "%s", time);
    snprintf(alarm->label, sizeof(alarm->label), "%s", label);
    snprintf(alarm->repeat, sizeof(alarm->repeat), "%s", repeat);
    alarm->enabled = enabled;
}

static void init_todo(app_todo_t *todo, const char *text, const char *tag, uint32_t tone, bool done)
{
    unsigned char random[16] = {0};
    FILE *file = fopen("/dev/urandom", "rb");
    bool random_ok = file && fread(random, 1, sizeof(random), file) == sizeof(random);
    if(file) fclose(file);
    if(!random_ok) {
        static uint64_t sequence;
        struct timespec clock; clock_gettime(CLOCK_MONOTONIC, &clock);
        uint64_t fallback[2] = {(uint64_t)clock.tv_sec * 1000000000ULL + (uint64_t)clock.tv_nsec, ++sequence};
        memcpy(random, fallback, sizeof(random));
    }
    for(size_t i = 0; i < sizeof(random); ++i) snprintf(todo->id + i * 2, 3, "%02x", random[i]);
    snprintf(todo->text, sizeof(todo->text), "%s", text);
    snprintf(todo->tag, sizeof(todo->tag), "%s", tag);
    todo->tone = tone;
    todo->done = done;
    todo->time[0] = '\0';
}

static void init_device(app_device_t *device, const char *name, const char *room,
                        uint32_t tone, bool enabled)
{
    snprintf(device->name, sizeof(device->name), "%s", name);
    snprintf(device->room, sizeof(device->room), "%s", room);
    device->tone = tone;
    device->enabled = enabled;
}

app_state_t *app_state_create(void)
{
    app_state_t *state = calloc(1, sizeof(*state));
    if(!state) return NULL;

    state->model.wifi = true;
    state->model.bluetooth = true;
    state->model.brightness = 72;
    state->model.volume = 45;
    state->model.screensaver = (app_screensaver_settings_t){.theme_id = "face", .standby_id = "pixel-room", .delay_seconds = 60};
    state->model.gestures = (app_gesture_settings_t){
        .enabled = true, .confirm_sensitive = true, .sensitivity = 1,
        .enabled_mask = 0x1b,
        .actions = {APP_GESTURE_PC_ON, APP_GESTURE_PLAY, APP_GESTURE_PLAY,
                    APP_GESTURE_PHOTO, APP_GESTURE_LAMP},
    };

    init_alarm(&state->model.alarms[0], "06:30", "晨间唤醒", "工作日", true);
    init_alarm(&state->model.alarms[1], "07:15", "出门提醒", "每天", true);
    init_alarm(&state->model.alarms[2], "13:00", "午休小睡", "仅一次", false);
    init_alarm(&state->model.alarms[3], "22:30", "准备睡觉", "每天", false);
    state->model.alarm_count = APP_ALARM_COUNT;
    state->model.alarms[0].days = 0x1f;
    state->model.alarms[1].days = 0x7f;
    state->model.alarms[3].days = 0x7f;

    init_todo(&state->model.todos[0], "完成 UI 设计评审", "工作", 0x5e5ce6, false);
    init_todo(&state->model.todos[1], "给绿萝浇水", "生活", 0x34c759, false);
    init_todo(&state->model.todos[2], "预约周五牙医", "健康", 0xff375f, true);
    init_todo(&state->model.todos[3], "阅读《设计心理学》两章", "学习", 0xff9500, false);
    init_todo(&state->model.todos[4], "回复客户邮件", "工作", 0x5e5ce6, true);
    state->model.todo_count = 5;

    init_device(&state->model.devices[0], "台灯", "书桌", 0xff9f0a, true);
    init_device(&state->model.devices[1], "加湿器", "书桌", 0x34c759, true);
    init_device(&state->model.devices[2], "屏幕挂灯", "书桌", 0x5e5ce6, false);
    init_device(&state->model.devices[3], "桌面风扇", "书桌", 0x30b0c7, false);
    state->model.computer = APP_COMPUTER_SLEEP;

    state->model.pomodoro.mode = APP_POMODORO_FOCUS;
    state->model.pomodoro.seconds_left = APP_POMODORO_FOCUS_SECONDS;
    state->model.pomodoro.focus_seconds = APP_POMODORO_FOCUS_SECONDS;
    state->model.pomodoro.rounds_completed = 0;
    state->model.vocabulary.cards = vocabulary_cards;
    state->model.vocabulary.card_count = APP_VOCABULARY_CAPACITY;
    state->model.vocabulary.current_index = 0;
    return state;
}

void app_state_destroy(app_state_t *state)
{
    free(state);
}

const app_model_t *app_state_model(const app_state_t *state)
{
    return state ? &state->model : NULL;
}

bool app_state_subscribe(app_state_t *state, app_state_listener_t listener, void *user_data)
{
    if(!state || !listener) return false;
    for(size_t i = 0; i < APP_LISTENER_CAPACITY; ++i) {
        if(!state->listeners[i].callback) {
            state->listeners[i].callback = listener;
            state->listeners[i].user_data = user_data;
            return true;
        }
    }
    return false;
}

void app_state_tick(app_state_t *state, uint32_t monotonic_ms)
{
    if(!state) return;
    uint32_t second = monotonic_ms / 1000U;
    if(second != state->last_clock_second) {
        state->last_clock_second = second;
        notify(state, APP_CHANGE_CLOCK);
    }

    if(state->model.pomodoro.running) {
        if(state->pomodoro_last_ms == 0) state->pomodoro_last_ms = monotonic_ms;
        uint32_t elapsed_seconds = (monotonic_ms - state->pomodoro_last_ms) / 1000U;
        if(elapsed_seconds > 0) {
            state->pomodoro_last_ms += elapsed_seconds * 1000U;
            bool transitioned = false;
            while(elapsed_seconds-- > 0) {
                if(state->model.pomodoro.seconds_left > 0) state->model.pomodoro.seconds_left--;
                if(state->model.pomodoro.seconds_left == 0) {
                    state->model.pomodoro.running = false;
                    transitioned = true;
                    if(state->model.pomodoro.mode == APP_POMODORO_FOCUS) {
                        state->model.pomodoro.rounds_completed++;
                        state->model.pomodoro.mode = APP_POMODORO_BREAK;
                        state->model.pomodoro.seconds_left = APP_POMODORO_BREAK_SECONDS;
                    }
                    else {
                        state->model.pomodoro.mode = APP_POMODORO_FOCUS;
                        state->model.pomodoro.seconds_left = state->model.pomodoro.focus_seconds;
                    }
                    break;
                }
            }
            notify(state, transitioned ? (APP_CHANGE_POMODORO |
                                          APP_CHANGE_POMODORO_PHASE)
                                       : APP_CHANGE_POMODORO_PROGRESS);
        }
    }

    if(state->model.computer == APP_COMPUTER_WAKING &&
       monotonic_ms - state->computer_wake_started_ms >= 1400U) {
        state->model.computer = APP_COMPUTER_ONLINE;
        notify(state, APP_CHANGE_DEVICES);
    }
}

void app_state_toggle_control(app_state_t *state, app_control_t control)
{
    if(!state) return;
    switch(control) {
        case APP_CONTROL_WIFI: state->model.wifi = !state->model.wifi; break;
        case APP_CONTROL_BLUETOOTH: state->model.bluetooth = !state->model.bluetooth; break;
        case APP_CONTROL_DND: state->model.dnd = !state->model.dnd; break;
    }
    notify(state, APP_CHANGE_CONTROLS);
}

void app_state_set_brightness(app_state_t *state, uint8_t value)
{
    if(!state) return;
    if(value < APP_BRIGHTNESS_MIN) value = APP_BRIGHTNESS_MIN;
    if(value > APP_BRIGHTNESS_MAX) value = APP_BRIGHTNESS_MAX;
    if(state->model.brightness == value) return;
    state->model.brightness = value;
    notify(state, APP_CHANGE_CONTROLS);
}

void app_state_set_volume(app_state_t *state, uint8_t value)
{
    if(!state) return;
    if(value > 100) value = 100;
    if(state->model.volume == value) return;
    state->model.volume = value;
    notify(state, APP_CHANGE_CONTROLS);
}

static bool screensaver_settings_valid(const app_screensaver_settings_t *settings)
{
    if(!settings || settings->delay_seconds > 3600 ||
       !memchr(settings->theme_id, 0, sizeof(settings->theme_id)) || !settings->theme_id[0]) return false;
    for(const unsigned char *p = (const unsigned char *)settings->theme_id; *p; ++p)
        if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    if(!memchr(settings->standby_id, 0, sizeof(settings->standby_id))) return false;
    for(const unsigned char *p = (const unsigned char *)settings->standby_id; *p; ++p)
        if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

bool app_state_set_screensaver(app_state_t *state, const app_screensaver_settings_t *settings)
{
    if(!state || !screensaver_settings_valid(settings)) return false;
    app_screensaver_settings_t *current = &state->model.screensaver;
    if(!strcmp(current->theme_id, settings->theme_id) &&
       !strcmp(current->standby_id, settings->standby_id) &&
       current->delay_seconds == settings->delay_seconds && current->shuffle == settings->shuffle) return true;
    *current = *settings;
    notify(state, APP_CHANGE_SCREENSAVER);
    return true;
}

void app_state_toggle_alarm(app_state_t *state, size_t index)
{
    if(!state || index >= state->model.alarm_count) return;
    state->model.alarms[index].enabled = !state->model.alarms[index].enabled;
    notify(state, APP_CHANGE_ALARMS);
}

void app_state_toggle_todo(app_state_t *state, size_t index)
{
    if(!state || index >= state->model.todo_count) return;
    state->model.todos[index].done = !state->model.todos[index].done;
    notify(state, APP_CHANGE_TODOS);
}

bool app_state_add_todo(app_state_t *state, const char *text)
{
    return app_state_add_todo_at_time(state, text, NULL);
}

static bool todo_time_valid(const char *time)
{
    if(!time || !time[0]) return true;
    if(strlen(time) != 5 || time[2] != ':') return false;
    for(size_t i = 0; i < 5; ++i) {
        if(i != 2 && (time[i] < '0' || time[i] > '9')) return false;
    }
    return (time[0] - '0') * 10 + time[1] - '0' < 24 &&
           (time[3] - '0') * 10 + time[4] - '0' < 60;
}

void app_state_format_alarm_repeat(uint8_t days, char *text, size_t capacity)
{
    if(!text || !capacity) return;
    if(days == 0 || days == 0x7f || days == 0x1f || days == 0x60) {
        snprintf(text, capacity, "%s", days == 0 ? "仅一次" : days == 0x7f ? "每天" :
                  days == 0x1f ? "工作日" : "周末");
        return;
    }
    static const char *weekdays[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    text[0] = '\0';
    for(unsigned i = 0; i < 7; ++i) {
        if(!(days & (1U << i))) continue;
        size_t length = strlen(text);
        snprintf(text + length, capacity - length, "%s%s", length ? " " : "", weekdays[i]);
    }
}

bool app_state_save_alarm(app_state_t *state, size_t index, const char *time,
                           const char *label, uint8_t days, bool enabled)
{
    if(!state || !time || strlen(time) != 5 || !todo_time_valid(time) || !label ||
       !label[0] || strlen(label) >= sizeof(state->model.alarms[0].label) || days > 0x7f ||
       index > state->model.alarm_count || index >= APP_ALARM_CAPACITY) return false;
    app_alarm_t *alarm = &state->model.alarms[index];
    snprintf(alarm->time, sizeof(alarm->time), "%s", time);
    snprintf(alarm->label, sizeof(alarm->label), "%s", label);
    alarm->days = days;
    alarm->enabled = enabled;
    app_state_format_alarm_repeat(days, alarm->repeat, sizeof(alarm->repeat));
    if(index == state->model.alarm_count) state->model.alarm_count++;
    notify(state, APP_CHANGE_ALARMS);
    return true;
}

bool app_state_delete_alarm(app_state_t *state, size_t index)
{
    if(!state || index >= state->model.alarm_count) return false;
    memmove(&state->model.alarms[index], &state->model.alarms[index + 1],
             (state->model.alarm_count - index - 1) * sizeof(state->model.alarms[0]));
    state->model.alarm_count--;
    memset(&state->model.alarms[state->model.alarm_count], 0, sizeof(state->model.alarms[0]));
    notify(state, APP_CHANGE_ALARMS);
    return true;
}

bool app_state_add_todo_at_time(app_state_t *state, const char *text, const char *time)
{
    if(!state || !text || !text[0] || !todo_time_valid(time) ||
       state->model.todo_count >= APP_TODO_CAPACITY) return false;
    init_todo(&state->model.todos[state->model.todo_count], text, "待办", 0x30b0c7, false);
    snprintf(state->model.todos[state->model.todo_count].time,
             sizeof(state->model.todos[0].time), "%s", time ? time : "");
    state->model.todo_count++;
    notify(state, APP_CHANGE_TODOS);
    return true;
}

bool app_state_set_todo_done(app_state_t *state, size_t index, bool done)
{
    if(!state || index >= state->model.todo_count) return false;
    if(state->model.todos[index].done != done) {
        state->model.todos[index].done = done;
        notify(state, APP_CHANGE_TODOS);
    }
    return true;
}

bool app_state_edit_todo(app_state_t *state, size_t index, const char *text)
{
    if(!state || index >= state->model.todo_count || !text || !text[0]) return false;
    snprintf(state->model.todos[index].text,
             sizeof(state->model.todos[index].text), "%s", text);
    notify(state, APP_CHANGE_TODOS);
    return true;
}

bool app_state_delete_todo(app_state_t *state, size_t index)
{
    if(!state || index >= state->model.todo_count) return false;
    if(index + 1U < state->model.todo_count) {
        memmove(&state->model.todos[index], &state->model.todos[index + 1U],
                (state->model.todo_count - index - 1U) * sizeof(state->model.todos[0]));
    }
    state->model.todo_count--;
    memset(&state->model.todos[state->model.todo_count], 0,
           sizeof(state->model.todos[0]));
    notify(state, APP_CHANGE_TODOS);
    return true;
}

void app_state_set_pomodoro_mode(app_state_t *state, app_pomodoro_mode_t mode)
{
    if(!state || (mode != APP_POMODORO_FOCUS && mode != APP_POMODORO_BREAK)) return;
    state->model.pomodoro.mode = mode;
    state->model.pomodoro.seconds_left = mode == APP_POMODORO_FOCUS
                                             ? state->model.pomodoro.focus_seconds
                                             : APP_POMODORO_BREAK_SECONDS;
    state->model.pomodoro.running = false;
    state->pomodoro_last_ms = 0;
    notify(state, APP_CHANGE_POMODORO);
}

void app_state_set_pomodoro_duration(app_state_t *state, uint32_t minutes)
{
    if(!state || minutes < 5 || minutes > 120) return;
    state->model.pomodoro.focus_seconds = minutes * 60U;
    app_state_set_pomodoro_mode(state, APP_POMODORO_FOCUS);
}

void app_state_start_pomodoro(app_state_t *state)
{
    if(!state || state->model.pomodoro.running) return;
    if(state->model.pomodoro.seconds_left == 0) {
        state->model.pomodoro.seconds_left = state->model.pomodoro.mode == APP_POMODORO_FOCUS
                                                 ? state->model.pomodoro.focus_seconds
                                                 : APP_POMODORO_BREAK_SECONDS;
    }
    state->model.pomodoro.running = true;
    state->pomodoro_last_ms = 0;
    notify(state, APP_CHANGE_POMODORO);
}

void app_state_pause_pomodoro(app_state_t *state)
{
    if(!state || !state->model.pomodoro.running) return;
    state->model.pomodoro.running = false;
    state->pomodoro_last_ms = 0;
    notify(state, APP_CHANGE_POMODORO);
}

void app_state_toggle_pomodoro(app_state_t *state)
{
    if(!state) return;
    if(state->model.pomodoro.running) app_state_pause_pomodoro(state);
    else app_state_start_pomodoro(state);
}

void app_state_reset_pomodoro(app_state_t *state)
{
    if(!state) return;
    app_state_set_pomodoro_mode(state, state->model.pomodoro.mode);
}

void app_state_set_microphone(app_state_t *state, bool enabled)
{
    if(!state || state->model.mic_muted == !enabled) return;
    state->model.mic_muted = !enabled;
    notify(state, APP_CHANGE_CONTROLS);
}

bool app_state_replace_todos(app_state_t *state, const app_todo_t *items, size_t count)
{
    if(!state || count > APP_TODO_CAPACITY || (count && !items)) return false;
    for(size_t i = 0; i < count; ++i) {
        if(!items[i].id[0] || !items[i].text[0] || strlen(items[i].id) >= sizeof(items[i].id) ||
           strlen(items[i].text) >= sizeof(items[i].text) ||
           (items[i].time[0] && !todo_time_valid(items[i].time))) return false;
        for(size_t j = 0; j < i; ++j) if(!strcmp(items[i].id, items[j].id)) return false;
    }
    memset(state->model.todos, 0, sizeof(state->model.todos));
    if(count) memcpy(state->model.todos, items, count * sizeof(*items));
    state->model.todo_count = count;
    notify(state, APP_CHANGE_TODOS);
    return true;
}

bool app_state_set_vocabulary(app_state_t *state, const app_vocabulary_card_t *cards,
                               size_t count, const app_vocabulary_t *progress)
{
    if(!state || count > APP_VOCABULARY_CAPACITY || (count && !cards)) return false;
    app_vocabulary_t next = progress ? *progress : (app_vocabulary_t){0};
    next.cards = cards;
    next.card_count = count;
    uint32_t full_mask = vocabulary_full_mask(count);
    next.known_mask &= full_mask;
    next.finished_mask = (next.finished_mask | next.known_mask) & full_mask;
    next.attempted_mask = (next.attempted_mask | next.finished_mask) & full_mask;
    if(next.current_index >= count) next.current_index = 0;
    next.complete = next.finished_mask == full_mask;
    state->model.vocabulary = next;
    notify(state, APP_CHANGE_VOCABULARY);
    return true;
}

void app_state_set_review_handler(app_state_t *state, app_vocabulary_review_fn handler, void *user_data)
{
    if(!state) return;
    state->review_handler = handler;
    state->review_user_data = user_data;
}

bool app_state_vocabulary_answer(app_state_t *state,
                                 app_vocabulary_answer_t answer)
{
    if(!state || !state->model.vocabulary.cards ||
       state->model.vocabulary.card_count == 0 ||
       (answer != APP_VOCABULARY_ANSWER_AGAIN &&
        answer != APP_VOCABULARY_ANSWER_FUZZY &&
        answer != APP_VOCABULARY_ANSWER_KNOWN)) {
        return false;
    }

    app_vocabulary_t *vocabulary = &state->model.vocabulary;
    const app_vocabulary_t previous = *vocabulary;
    const uint32_t full_mask = vocabulary_full_mask(vocabulary->card_count);
    if(vocabulary->complete) {
        vocabulary->known_mask = 0;
        vocabulary->attempted_mask = 0;
        vocabulary->finished_mask = 0;
        vocabulary->reviewed_count = 0;
        vocabulary->correct_count = 0;
        vocabulary->complete = false;
        vocabulary->current_index = 0;
    }

    size_t index = vocabulary->current_index;
    if(index >= vocabulary->card_count) index = 0;
    uint32_t bit = 1U << index;
    bool retry = (vocabulary->attempted_mask & bit) != 0;
    vocabulary->attempted_mask |= bit;
    if(retry || answer == APP_VOCABULARY_ANSWER_KNOWN) vocabulary->finished_mask |= bit;
    if(vocabulary->reviewed_count < UINT16_MAX) vocabulary->reviewed_count++;
    if(answer == APP_VOCABULARY_ANSWER_KNOWN) {
        vocabulary->known_mask |= bit;
        if(vocabulary->correct_count < UINT16_MAX) vocabulary->correct_count++;
    }
    else {
        vocabulary->known_mask &= ~bit;
    }

    bool completed = full_mask != 0 &&
                     vocabulary->finished_mask == full_mask;
    if(completed) {
        vocabulary->complete = true;
        vocabulary->current_index = 0;
        if(vocabulary->cycles_completed < UINT16_MAX) vocabulary->cycles_completed++;
    }
    else {
        vocabulary->current_index = vocabulary_next_card(vocabulary, index);
    }
    if(state->review_handler && !state->review_handler(&vocabulary->cards[index],
         answer, retry, vocabulary, state->review_user_data)) {
        *vocabulary = previous;
        return false;
    }
    notify(state, APP_CHANGE_VOCABULARY);
    return completed;
}

void app_state_vocabulary_repeat(app_state_t *state)
{
    if(!state || !state->model.vocabulary.cards ||
       state->model.vocabulary.card_count == 0) return;
    app_vocabulary_t *vocabulary = &state->model.vocabulary;
    if(vocabulary->complete) {
        vocabulary->known_mask = 0;
        vocabulary->attempted_mask = 0;
        vocabulary->finished_mask = 0;
        vocabulary->reviewed_count = 0;
        vocabulary->correct_count = 0;
        vocabulary->complete = false;
        vocabulary->current_index = 0;
    }
    else if(vocabulary->current_index < vocabulary->card_count) {
        vocabulary->known_mask &= ~(1U << vocabulary->current_index);
        vocabulary->finished_mask &= ~(1U << vocabulary->current_index);
    }
    notify(state, APP_CHANGE_VOCABULARY);
}

void app_state_vocabulary_reset(app_state_t *state)
{
    if(!state || !state->model.vocabulary.cards) return;
    app_vocabulary_t *vocabulary = &state->model.vocabulary;
    vocabulary->current_index = 0;
    vocabulary->known_mask = 0;
    vocabulary->attempted_mask = 0;
    vocabulary->finished_mask = 0;
    vocabulary->reviewed_count = 0;
    vocabulary->correct_count = 0;
    vocabulary->complete = false;
    notify(state, APP_CHANGE_VOCABULARY);
}

void app_state_toggle_device(app_state_t *state, size_t index)
{
    if(!state || index >= APP_DEVICE_COUNT) return;
    bool enabled = !state->model.devices[index].enabled;
    state->model.devices[index].enabled = enabled;
    if(enabled && index == 0) state->model.devices[1].enabled = true;
    if(enabled && index == 2) state->model.devices[3].enabled = true;
    notify(state, APP_CHANGE_DEVICES);
}

void app_state_select_computer(app_state_t *state, uint32_t monotonic_ms)
{
    if(!state || state->model.computer != APP_COMPUTER_SLEEP) return;
    state->model.computer = APP_COMPUTER_SELECTED;
    state->computer_wake_started_ms = monotonic_ms;
    notify(state, APP_CHANGE_DEVICES);
}

void app_state_set_device(app_state_t *state, size_t index, bool enabled)
{
    if(!state || index >= APP_DEVICE_COUNT || state->model.devices[index].enabled == enabled) return;
    state->model.devices[index].enabled = enabled;
    notify(state, APP_CHANGE_DEVICES);
}

void app_state_sleep_computer(app_state_t *state)
{
    if(!state || state->model.computer == APP_COMPUTER_SLEEP) return;
    state->model.computer = APP_COMPUTER_SLEEP;
    notify(state, APP_CHANGE_DEVICES);
}

void app_state_confirm_computer_wake(app_state_t *state, uint32_t monotonic_ms)
{
    if(!state || state->model.computer != APP_COMPUTER_SELECTED) return;
    state->model.computer = APP_COMPUTER_WAKING;
    state->computer_wake_started_ms = monotonic_ms;
    notify(state, APP_CHANGE_DEVICES);
}

bool app_state_set_gestures(app_state_t *state, const app_gesture_settings_t *settings)
{
    if(!state || !settings || settings->sensitivity > 2 ||
       settings->enabled_mask >= (1U << APP_GESTURE_COUNT)) return false;
    for(size_t i = 0; i < APP_GESTURE_COUNT; ++i)
        if(settings->actions[i] >= APP_GESTURE_ACTION_COUNT) return false;
    app_gesture_settings_t *current = &state->model.gestures;
    if(current->enabled == settings->enabled &&
       current->confirm_sensitive == settings->confirm_sensitive &&
       current->sensitivity == settings->sensitivity &&
       current->enabled_mask == settings->enabled_mask &&
       !memcmp(current->actions, settings->actions, sizeof(current->actions))) return true;
    *current = *settings;
    notify(state, APP_CHANGE_GESTURES);
    return true;
}

void app_state_export_persisted(const app_state_t *state, app_persisted_state_t *persisted)
{
    if(!state || !persisted) return;
    memset(persisted, 0, sizeof(*persisted));
    persisted->brightness = state->model.brightness;
    persisted->volume = state->model.volume;
    persisted->gestures_present = true;
    persisted->gestures = state->model.gestures;
    persisted->screensaver_present = true;
    persisted->screensaver = state->model.screensaver;
    persisted->control_flags = (state->model.wifi ? 1U : 0U) |
                               (state->model.bluetooth ? 2U : 0U) |
                               (state->model.dnd ? 4U : 0U) |
                               (state->model.mic_muted ? 8U : 0U);
    persisted->alarm_content_present = true;
    persisted->alarm_count = (uint8_t)state->model.alarm_count;
    for(size_t i = 0; i < state->model.alarm_count; ++i) {
        if(state->model.alarms[i].enabled) persisted->alarm_mask |= (uint8_t)(1U << i);
        snprintf(persisted->alarm_time[i], sizeof(persisted->alarm_time[i]), "%s", state->model.alarms[i].time);
        snprintf(persisted->alarm_label[i], sizeof(persisted->alarm_label[i]), "%s", state->model.alarms[i].label);
        persisted->alarm_days[i] = state->model.alarms[i].days;
    }
    for(size_t i = 0; i < state->model.todo_count && i < 8; ++i) {
        if(state->model.todos[i].done) persisted->todo_done_mask |= (uint8_t)(1U << i);
        snprintf(persisted->todo_text[i], sizeof(persisted->todo_text[i]), "%s",
                 state->model.todos[i].text);
        snprintf(persisted->todo_id[i], sizeof(persisted->todo_id[i]), "%s", state->model.todos[i].id);
        snprintf(persisted->todo_time[i], sizeof(persisted->todo_time[i]), "%s",
                 state->model.todos[i].time);
    }
    persisted->todo_count = (uint8_t)state->model.todo_count;
    persisted->todo_content_present = true;
    for(size_t i = 0; i < APP_DEVICE_COUNT; ++i) {
        if(state->model.devices[i].enabled) persisted->device_mask |= (uint8_t)(1U << i);
    }
    persisted->computer_state = (uint8_t)state->model.computer;
    persisted->pomodoro_rounds = (uint16_t)state->model.pomodoro.rounds_completed;
    persisted->pomodoro_mode = (uint8_t)state->model.pomodoro.mode;
    persisted->pomodoro_seconds = state->model.pomodoro.seconds_left;
    persisted->pomodoro_focus_seconds = state->model.pomodoro.focus_seconds;
    persisted->vocabulary_known_mask = state->model.vocabulary.known_mask;
    persisted->vocabulary_index = (uint8_t)state->model.vocabulary.current_index;
    persisted->vocabulary_reviewed = state->model.vocabulary.reviewed_count;
    persisted->vocabulary_correct = state->model.vocabulary.correct_count;
    persisted->vocabulary_cycles = state->model.vocabulary.cycles_completed;
}

void app_state_import_persisted(app_state_t *state, const app_persisted_state_t *persisted)
{
    if(!state || !persisted) return;
    if(persisted->brightness > APP_BRIGHTNESS_MAX) {
        state->model.brightness = 72;
    }
    else if(persisted->brightness < APP_BRIGHTNESS_MIN) {
        /* Migrate releases that persisted a zero brightness to the new
         * hardware-safe minimum instead of restoring a brighter default. */
        state->model.brightness = APP_BRIGHTNESS_MIN;
    }
    else {
        state->model.brightness = persisted->brightness;
    }
    state->model.volume = persisted->volume <= 100 ? persisted->volume : 45;
    if(persisted->screensaver_present && screensaver_settings_valid(&persisted->screensaver))
        state->model.screensaver = persisted->screensaver;
    if(persisted->gestures_present) {
        const app_gesture_settings_t *g = &persisted->gestures;
        bool valid = g->sensitivity <= 2 && g->enabled_mask < (1U << APP_GESTURE_COUNT);
        for(size_t i = 0; i < APP_GESTURE_COUNT; ++i)
            if(g->actions[i] >= APP_GESTURE_ACTION_COUNT) valid = false;
        if(valid) state->model.gestures = *g;
    }
    state->model.wifi = (persisted->control_flags & 1U) != 0;
    state->model.bluetooth = (persisted->control_flags & 2U) != 0;
    state->model.dnd = (persisted->control_flags & 4U) != 0;
    state->model.mic_muted = (persisted->control_flags & 8U) != 0;
    if(persisted->alarm_content_present && persisted->alarm_count <= APP_ALARM_CAPACITY) {
        bool valid = true;
        for(size_t i = 0; i < persisted->alarm_count; ++i) {
            if(strlen(persisted->alarm_time[i]) != 5 || !todo_time_valid(persisted->alarm_time[i]) ||
               !persisted->alarm_label[i][0] || persisted->alarm_days[i] > 0x7f) valid = false;
        }
        if(valid) {
            memset(state->model.alarms, 0, sizeof(state->model.alarms));
            state->model.alarm_count = persisted->alarm_count;
            for(size_t i = 0; i < state->model.alarm_count; ++i) {
                app_alarm_t *alarm = &state->model.alarms[i];
                snprintf(alarm->time, sizeof(alarm->time), "%s", persisted->alarm_time[i]);
                snprintf(alarm->label, sizeof(alarm->label), "%s", persisted->alarm_label[i]);
                alarm->days = persisted->alarm_days[i];
                app_state_format_alarm_repeat(alarm->days, alarm->repeat, sizeof(alarm->repeat));
            }
        }
    }
    for(size_t i = 0; i < state->model.alarm_count; ++i) {
        state->model.alarms[i].enabled = (persisted->alarm_mask & (1U << i)) != 0;
    }
    if(persisted->todo_content_present && persisted->todo_count <= APP_TODO_CAPACITY) {
        state->model.todo_count = persisted->todo_count;
        for(size_t i = 0; i < state->model.todo_count; ++i) {
            init_todo(&state->model.todos[i], persisted->todo_text[i],
                      "待办", 0x30b0c7,
                      (persisted->todo_done_mask & (1U << i)) != 0);
            if(persisted->todo_id[i][0])
                snprintf(state->model.todos[i].id, sizeof(state->model.todos[i].id), "%s", persisted->todo_id[i]);
            if(todo_time_valid(persisted->todo_time[i]))
                snprintf(state->model.todos[i].time, sizeof(state->model.todos[i].time),
                         "%s", persisted->todo_time[i]);
        }
        for(size_t i = state->model.todo_count; i < APP_TODO_CAPACITY; ++i) {
            memset(&state->model.todos[i], 0, sizeof(state->model.todos[i]));
        }
    }
    for(size_t i = 0; i < state->model.todo_count && i < 8; ++i) {
        state->model.todos[i].done = (persisted->todo_done_mask & (1U << i)) != 0;
    }
    for(size_t i = 0; i < APP_DEVICE_COUNT; ++i) {
        state->model.devices[i].enabled = (persisted->device_mask & (1U << i)) != 0;
    }
    state->model.computer = persisted->computer_state <= APP_COMPUTER_ONLINE
                                ? (app_computer_state_t)persisted->computer_state
                                : APP_COMPUTER_SLEEP;
    state->model.pomodoro.rounds_completed = persisted->pomodoro_rounds;
    if(persisted->pomodoro_mode <= APP_POMODORO_BREAK) {
        state->model.pomodoro.mode = (app_pomodoro_mode_t)persisted->pomodoro_mode;
    }
    state->model.pomodoro.focus_seconds =
        persisted->pomodoro_focus_seconds >= 300 && persisted->pomodoro_focus_seconds <= 7200
            ? persisted->pomodoro_focus_seconds : APP_POMODORO_FOCUS_SECONDS;
    uint32_t default_seconds = state->model.pomodoro.mode == APP_POMODORO_FOCUS
                                   ? state->model.pomodoro.focus_seconds
                                   : APP_POMODORO_BREAK_SECONDS;
    state->model.pomodoro.seconds_left =
        persisted->pomodoro_seconds > 0 && persisted->pomodoro_seconds <= default_seconds
            ? persisted->pomodoro_seconds
            : default_seconds;
    state->model.pomodoro.running = false;
    state->pomodoro_last_ms = 0;

    app_vocabulary_t *vocabulary = &state->model.vocabulary;
    uint32_t full_mask = vocabulary_full_mask(vocabulary->card_count);
    vocabulary->known_mask = persisted->vocabulary_known_mask & full_mask;
    vocabulary->attempted_mask = vocabulary->known_mask;
    vocabulary->finished_mask = vocabulary->known_mask;
    vocabulary->current_index = persisted->vocabulary_index < vocabulary->card_count
                                    ? persisted->vocabulary_index
                                    : 0;
    vocabulary->reviewed_count = persisted->vocabulary_reviewed;
    vocabulary->correct_count = persisted->vocabulary_correct;
    vocabulary->cycles_completed = persisted->vocabulary_cycles;
    vocabulary->complete = full_mask != 0 && vocabulary->known_mask == full_mask;
    if(vocabulary->complete) vocabulary->current_index = 0;
    notify(state, APP_CHANGE_ALL);
}
