#ifndef BINBOT_APP_STATE_H
#define BINBOT_APP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    APP_ALARM_COUNT = 4,
    APP_ALARM_CAPACITY = 8,
    APP_TODO_CAPACITY = 8,
    APP_DEVICE_COUNT = 4,
    APP_VOCABULARY_CAPACITY = 30,
    APP_POMODORO_FOCUS_SECONDS = 25U * 60U,
    APP_POMODORO_BREAK_SECONDS = 5U * 60U,
    APP_BRIGHTNESS_MIN = 1,
    APP_BRIGHTNESS_MAX = 100,
};

typedef enum {
    APP_CHANGE_NONE = 0,
    APP_CHANGE_CLOCK = 1U << 0,
    APP_CHANGE_CONTROLS = 1U << 1,
    APP_CHANGE_ALARMS = 1U << 2,
    APP_CHANGE_POMODORO = 1U << 3,
    APP_CHANGE_TODOS = 1U << 4,
    APP_CHANGE_DEVICES = 1U << 5,
    /* Countdown-only updates are intentionally not persisted. */
    APP_CHANGE_POMODORO_PROGRESS = 1U << 6,
    APP_CHANGE_VOCABULARY = 1U << 7,
    /* A focus/break period reached zero and the next period was prepared. */
    APP_CHANGE_POMODORO_PHASE = 1U << 8,
    APP_CHANGE_CALENDAR = 1U << 9,
    APP_CHANGE_GESTURES = 1U << 10,
    APP_CHANGE_SCREENSAVER = 1U << 11,
} app_change_t;

#define APP_CHANGE_ALL UINT32_MAX

typedef enum {
    APP_CONTROL_WIFI,
    APP_CONTROL_BLUETOOTH,
    APP_CONTROL_DND,
} app_control_t;

typedef enum {
    APP_POMODORO_FOCUS,
    APP_POMODORO_BREAK,
} app_pomodoro_mode_t;

typedef enum {
    APP_VOCABULARY_ANSWER_AGAIN,
    APP_VOCABULARY_ANSWER_KNOWN,
    APP_VOCABULARY_ANSWER_FUZZY,
} app_vocabulary_answer_t;

typedef enum {
    APP_COMPUTER_SLEEP,
    APP_COMPUTER_SELECTED,
    APP_COMPUTER_WAKING,
    APP_COMPUTER_ONLINE,
} app_computer_state_t;

typedef struct {
    char time[6];
    char label[32];
    char repeat[48];
    uint8_t days;
    bool enabled;
} app_alarm_t;

typedef struct {
    char text[72];
    char id[65];
    char tag[16];
    char time[6];
    uint32_t tone;
    bool done;
} app_todo_t;

typedef struct {
    char name[28];
    char room[20];
    uint32_t tone;
    bool enabled;
} app_device_t;

typedef struct {
    app_pomodoro_mode_t mode;
    uint32_t seconds_left;
    uint32_t focus_seconds;
    uint32_t rounds_completed;
    bool running;
} app_pomodoro_t;

typedef struct {
    const char *word;
    const char *phonetic;
    const char *meaning;
    const char *example;
    const char *translation;
} app_vocabulary_card_t;

typedef struct {
    bool available;
    uint16_t due;
    uint16_t new_count;
    uint16_t learned_today;
    uint32_t next_review_at;
    uint32_t new_mask;
} app_vocabulary_plan_t;

typedef struct {
    const app_vocabulary_card_t *cards;
    size_t card_count;
    size_t current_index;
    uint32_t known_mask;
    uint16_t reviewed_count;
    uint16_t correct_count;
    uint16_t cycles_completed;
    bool complete;
    uint32_t attempted_mask;
    uint32_t finished_mask;
    app_vocabulary_plan_t plan;
} app_vocabulary_t;
typedef bool (*app_vocabulary_review_fn)(const app_vocabulary_card_t *card,
                                         app_vocabulary_answer_t answer, bool retry,
                                         const app_vocabulary_t *progress, void *user_data);

typedef enum {
    APP_GESTURE_OK, APP_GESTURE_THUMB, APP_GESTURE_PALM,
    APP_GESTURE_VICTORY, APP_GESTURE_FIST, APP_GESTURE_COUNT,
} app_gesture_id_t;

typedef enum {
    APP_GESTURE_PC_ON, APP_GESTURE_PC_SLEEP, APP_GESTURE_LAMP,
    APP_GESTURE_PLAY, APP_GESTURE_NEXT, APP_GESTURE_PREVIOUS,
    APP_GESTURE_PHOTO, APP_GESTURE_FOCUS, APP_GESTURE_WAKE,
    APP_GESTURE_NOISE, APP_GESTURE_MUTE, APP_GESTURE_NOTHING,
    APP_GESTURE_ACTION_COUNT,
} app_gesture_action_t;

typedef struct {
    bool enabled;
    bool confirm_sensitive;
    uint8_t sensitivity;
    uint8_t enabled_mask;
    uint8_t actions[APP_GESTURE_COUNT];
} app_gesture_settings_t;

typedef struct {
    char theme_id[32];
    char standby_id[32]; /* Remember the dial when Expressions is selected. */
    uint16_t delay_seconds; /* Zero disables automatic entry. */
    bool shuffle;
} app_screensaver_settings_t;

typedef struct {
    bool wifi;
    bool bluetooth;
    bool dnd;
    bool mic_muted;
    uint8_t brightness;
    uint8_t volume;
    app_alarm_t alarms[APP_ALARM_CAPACITY];
    size_t alarm_count;
    app_todo_t todos[APP_TODO_CAPACITY];
    size_t todo_count;
    app_device_t devices[APP_DEVICE_COUNT];
    app_computer_state_t computer;
    app_pomodoro_t pomodoro;
    app_vocabulary_t vocabulary;
    app_gesture_settings_t gestures;
    app_screensaver_settings_t screensaver;
} app_model_t;

typedef struct {
    uint8_t brightness;
    uint8_t volume;
    uint8_t control_flags;
    uint8_t alarm_mask;
    uint8_t alarm_count;
    bool alarm_content_present;
    char alarm_time[APP_ALARM_CAPACITY][6];
    char alarm_label[APP_ALARM_CAPACITY][32];
    uint8_t alarm_days[APP_ALARM_CAPACITY];
    uint8_t todo_done_mask;
    uint8_t todo_count;
    bool todo_content_present;
    char todo_text[APP_TODO_CAPACITY][72];
    char todo_id[APP_TODO_CAPACITY][65];
    char todo_time[APP_TODO_CAPACITY][6];
    uint8_t device_mask;
    uint8_t computer_state;
    uint16_t pomodoro_rounds;
    uint8_t pomodoro_mode;
    uint32_t pomodoro_seconds;
    uint32_t pomodoro_focus_seconds;
    uint32_t vocabulary_known_mask;
    uint8_t vocabulary_index;
    uint16_t vocabulary_reviewed;
    uint16_t vocabulary_correct;
    uint16_t vocabulary_cycles;
    bool gestures_present;
    app_gesture_settings_t gestures;
    bool screensaver_present;
    app_screensaver_settings_t screensaver;
} app_persisted_state_t;

typedef struct app_state app_state_t;
typedef void (*app_state_listener_t)(uint32_t changes, const app_model_t *model, void *user_data);

app_state_t *app_state_create(void);
void app_state_destroy(app_state_t *state);
const app_model_t *app_state_model(const app_state_t *state);
bool app_state_subscribe(app_state_t *state, app_state_listener_t listener, void *user_data);
void app_state_tick(app_state_t *state, uint32_t monotonic_ms);

void app_state_toggle_control(app_state_t *state, app_control_t control);
void app_state_set_brightness(app_state_t *state, uint8_t value);
void app_state_set_volume(app_state_t *state, uint8_t value);
void app_state_set_microphone(app_state_t *state, bool enabled);
bool app_state_replace_todos(app_state_t *state, const app_todo_t *items, size_t count);
bool app_state_set_vocabulary(app_state_t *state, const app_vocabulary_card_t *cards,
                               size_t count, const app_vocabulary_t *progress);
void app_state_set_review_handler(app_state_t *state, app_vocabulary_review_fn handler, void *user_data);
bool app_state_set_gestures(app_state_t *state, const app_gesture_settings_t *settings);
bool app_state_set_screensaver(app_state_t *state, const app_screensaver_settings_t *settings);
void app_state_toggle_alarm(app_state_t *state, size_t index);
bool app_state_save_alarm(app_state_t *state, size_t index, const char *time,
                           const char *label, uint8_t days, bool enabled);
bool app_state_delete_alarm(app_state_t *state, size_t index);
void app_state_format_alarm_repeat(uint8_t days, char *text, size_t capacity);
void app_state_toggle_todo(app_state_t *state, size_t index);
bool app_state_add_todo(app_state_t *state, const char *text);
bool app_state_add_todo_at_time(app_state_t *state, const char *text, const char *time);
bool app_state_set_todo_done(app_state_t *state, size_t index, bool done);
bool app_state_edit_todo(app_state_t *state, size_t index, const char *text);
bool app_state_delete_todo(app_state_t *state, size_t index);
void app_state_set_pomodoro_mode(app_state_t *state, app_pomodoro_mode_t mode);
void app_state_set_pomodoro_duration(app_state_t *state, uint32_t minutes);
void app_state_start_pomodoro(app_state_t *state);
void app_state_pause_pomodoro(app_state_t *state);
void app_state_toggle_pomodoro(app_state_t *state);
void app_state_reset_pomodoro(app_state_t *state);
bool app_state_vocabulary_answer(app_state_t *state,
                                 app_vocabulary_answer_t answer);
void app_state_vocabulary_repeat(app_state_t *state);
void app_state_vocabulary_reset(app_state_t *state);
void app_state_toggle_device(app_state_t *state, size_t index);
void app_state_set_device(app_state_t *state, size_t index, bool enabled);
void app_state_sleep_computer(app_state_t *state);
void app_state_select_computer(app_state_t *state, uint32_t monotonic_ms);
void app_state_confirm_computer_wake(app_state_t *state, uint32_t monotonic_ms);

void app_state_export_persisted(const app_state_t *state, app_persisted_state_t *persisted);
void app_state_import_persisted(app_state_t *state, const app_persisted_state_t *persisted);

#ifdef __cplusplus
}
#endif

#endif
