#include "binbot/app_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { STATE_SCHEMA_VERSION = 6 };

struct app_runtime {
    app_state_t *state;
    char state_path[256];
    bool dirty;
    uint32_t dirty_since_ms;
};

static void state_changed(uint32_t changes, const app_model_t *model, void *user_data)
{
    (void)model;
    app_runtime_t *runtime = user_data;
    const uint32_t persisted = APP_CHANGE_CONTROLS | APP_CHANGE_ALARMS |
                               APP_CHANGE_POMODORO | APP_CHANGE_TODOS |
                               APP_CHANGE_POMODORO_PHASE | APP_CHANGE_DEVICES |
                               APP_CHANGE_VOCABULARY | APP_CHANGE_GESTURES |
                               APP_CHANGE_SCREENSAVER;
    if(changes & persisted) runtime->dirty = true;
}

static void ensure_parent_directory(const char *path)
{
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if(!slash || slash == copy) return;
    *slash = '\0';
    if(mkdir(copy, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "runtime: cannot create state directory %s: %s\n", copy, strerror(errno));
    }
}

static bool hex_decode(const char *input, char *output, size_t capacity)
{
    size_t length = strlen(input);
    if((length & 1U) != 0 || length / 2U >= capacity) return false;
    for(size_t i = 0; i < length / 2U; ++i) {
        unsigned value;
        if(sscanf(input + i * 2U, "%2x", &value) != 1) return false;
        output[i] = (char)value;
    }
    output[length / 2U] = '\0';
    return true;
}

static void write_hex(FILE *file, const char *key, const char *value)
{
    static const char digits[] = "0123456789abcdef";
    fprintf(file, "%s=", key);
    for(size_t i = 0; value[i]; ++i) {
        const unsigned char byte = (unsigned char)value[i];
        fputc(digits[byte >> 4], file);
        fputc(digits[byte & 0x0fU], file);
    }
    fputc('\n', file);
}

static bool load_state(app_runtime_t *runtime)
{
    FILE *file = fopen(runtime->state_path, "r");
    if(!file) return false;

    app_persisted_state_t value = {0};
    app_state_export_persisted(runtime->state, &value);
    unsigned schema = 0;
    bool standby_present = false, saver_migrated = false;
    char line[256];
    while(fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *separator = strchr(line, '=');
        if(!separator) continue;
        *separator = '\0';
        const char *key = line;
        const char *text = separator + 1;
        if(strcmp(key, "screensaver_theme") == 0) {
            if(strlen(text) < sizeof(value.screensaver.theme_id))
                snprintf(value.screensaver.theme_id, sizeof(value.screensaver.theme_id), "%s", text);
            continue;
        }
        if(strcmp(key, "screensaver_standby") == 0) {
            if(strlen(text) < sizeof(value.screensaver.standby_id)) {
                snprintf(value.screensaver.standby_id, sizeof(value.screensaver.standby_id), "%s", text);
                standby_present = true;
            }
            continue;
        }
        if(strncmp(key, "alarm_time_", 11) == 0 || strncmp(key, "alarm_label_", 12) == 0 ||
           strncmp(key, "alarm_days_", 11) == 0) {
            bool label = strncmp(key, "alarm_label_", 12) == 0;
            bool days = strncmp(key, "alarm_days_", 11) == 0;
            char *end = NULL;
            unsigned long index = strtoul(key + (label ? 12 : 11), &end, 10);
            if(end && !*end && index < APP_ALARM_CAPACITY) {
                if(days) {
                    unsigned long mask = strtoul(text, &end, 10);
                    if(end && !*end && mask <= 0x7f) value.alarm_days[index] = (uint8_t)mask;
                }
                else if(label) (void)hex_decode(text, value.alarm_label[index], sizeof(value.alarm_label[index]));
                else (void)hex_decode(text, value.alarm_time[index], sizeof(value.alarm_time[index]));
            }
            continue;
        }
        if(strncmp(key, "todo_id_", 8) == 0) {
            unsigned index = (unsigned)strtoul(key + 8, NULL, 10);
            if(index < APP_TODO_CAPACITY)
                (void)hex_decode(text, value.todo_id[index], sizeof(value.todo_id[index]));
            continue;
        }
        if(strncmp(key, "todo_time_", 10) == 0) {
            char *end = NULL;
            unsigned long index = strtoul(key + 10, &end, 10);
            if(end && !*end && index < APP_TODO_CAPACITY)
                (void)hex_decode(text, value.todo_time[index], sizeof(value.todo_time[index]));
            continue;
        }
        if(strncmp(key, "todo_", 5) == 0 && strcmp(key, "todo_count") != 0) {
            char *end = NULL;
            unsigned long index = strtoul(key + 5, &end, 10);
            if(end && !*end && index < APP_TODO_CAPACITY) {
                (void)hex_decode(text, value.todo_text[index],
                                 sizeof(value.todo_text[index]));
            }
            continue;
        }
        char *end = NULL;
        unsigned long number = strtoul(text, &end, 10);
        if(!end || *end) continue;
        if(strcmp(key, "schema") == 0) schema = (unsigned)number;
        else if(strcmp(key, "brightness") == 0) value.brightness = (uint8_t)number;
        else if(strcmp(key, "volume") == 0) value.volume = (uint8_t)number;
        else if(strcmp(key, "screensaver_delay") == 0 && number <= 3600)
            value.screensaver.delay_seconds = (uint16_t)number;
        else if(strcmp(key, "screensaver_shuffle") == 0 && number <= 1)
            value.screensaver.shuffle = number != 0;
        else if(strcmp(key, "gesture_enabled") == 0 && number <= 1) value.gestures.enabled = number != 0;
        else if(strcmp(key, "gesture_confirm") == 0 && number <= 1) value.gestures.confirm_sensitive = number != 0;
        else if(strcmp(key, "gesture_sensitivity") == 0 && number <= 2) value.gestures.sensitivity = (uint8_t)number;
        else if(strcmp(key, "gesture_mask") == 0 && number < (1U << APP_GESTURE_COUNT)) value.gestures.enabled_mask = (uint8_t)number;
        else if(strncmp(key, "gesture_action_", 15) == 0 && number < APP_GESTURE_ACTION_COUNT) {
            char *index_end = NULL;
            unsigned long index = strtoul(key + 15, &index_end, 10);
            if(index_end != key + 15 && !*index_end && index < APP_GESTURE_COUNT)
                value.gestures.actions[index] = (uint8_t)number;
        }
        else if(strcmp(key, "controls") == 0) value.control_flags = (uint8_t)number;
        else if(strcmp(key, "alarms") == 0) value.alarm_mask = (uint8_t)number;
        else if(strcmp(key, "alarm_count") == 0) value.alarm_count = (uint8_t)number;
        else if(strcmp(key, "todos") == 0) value.todo_done_mask = (uint8_t)number;
        else if(strcmp(key, "todo_count") == 0) value.todo_count = (uint8_t)number;
        else if(strcmp(key, "devices") == 0) value.device_mask = (uint8_t)number;
        else if(strcmp(key, "computer") == 0) value.computer_state = (uint8_t)number;
        else if(strcmp(key, "pomodoro_rounds") == 0) value.pomodoro_rounds = (uint16_t)number;
        else if(strcmp(key, "pomodoro_mode") == 0) value.pomodoro_mode = (uint8_t)number;
        else if(strcmp(key, "pomodoro_seconds") == 0) value.pomodoro_seconds = (uint32_t)number;
        else if(strcmp(key, "pomodoro_focus_seconds") == 0) value.pomodoro_focus_seconds = (uint32_t)number;
        else if(strcmp(key, "vocabulary_known_mask") == 0) value.vocabulary_known_mask = (uint32_t)number;
        else if(strcmp(key, "vocabulary_index") == 0) value.vocabulary_index = (uint8_t)number;
        else if(strcmp(key, "vocabulary_reviewed") == 0) value.vocabulary_reviewed = (uint16_t)number;
        else if(strcmp(key, "vocabulary_correct") == 0) value.vocabulary_correct = (uint16_t)number;
        else if(strcmp(key, "vocabulary_cycles") == 0) value.vocabulary_cycles = (uint16_t)number;
    }
    fclose(file);
    /* Schema 1/2 files remain readable; new fields keep their defaults. */
    if(schema < 1 || schema > STATE_SCHEMA_VERSION) return false;
    value.todo_content_present = schema >= 3;
    value.alarm_content_present = schema >= 4;
    value.gestures_present = schema >= 5;
    value.screensaver_present = schema >= 6;
    static const char *const legacy_savers[][2] = {
        {"clock", "shapes"}, {"weather", "circuit-core"},
        {"terminal", "ban-shell"}, {"focus", "hamster-wheel"},
        {"green-screen", "ban-shell"}, {"orbit-deck", "circuit-core"},
        {"warm-radio", "dial-radio"}, {"amber-clock", "nixie-tubes"}
    };
    if(value.screensaver_present) {
        for(size_t i = 0; i < sizeof(legacy_savers) / sizeof(legacy_savers[0]); ++i) {
            if(!strcmp(value.screensaver.theme_id, legacy_savers[i][0])) {
                snprintf(value.screensaver.theme_id, sizeof(value.screensaver.theme_id), "%s", legacy_savers[i][1]);
                saver_migrated = true;
            }
            if(!strcmp(value.screensaver.standby_id, legacy_savers[i][0])) {
                snprintf(value.screensaver.standby_id, sizeof(value.screensaver.standby_id), "%s", legacy_savers[i][1]);
                saver_migrated = true;
            }
        }
        if(!standby_present && strcmp(value.screensaver.theme_id, "face"))
            snprintf(value.screensaver.standby_id, sizeof(value.screensaver.standby_id), "%s", value.screensaver.theme_id);
    }
    bool brightness_needs_migration = value.brightness < APP_BRIGHTNESS_MIN ||
                                      value.brightness > APP_BRIGHTNESS_MAX;
    app_state_import_persisted(runtime->state, &value);
    /* Persist the normalized minimum/default so a legacy zero cannot return
     * on every subsequent restart. */
    runtime->dirty = brightness_needs_migration || saver_migrated;
    fprintf(stdout, "runtime: restored state schema %u from %s\n", schema, runtime->state_path);
    return true;
}

static bool save_state(app_runtime_t *runtime)
{
    ensure_parent_directory(runtime->state_path);
    char temporary[288];
    snprintf(temporary, sizeof(temporary), "%s.tmp", runtime->state_path);
    FILE *file = fopen(temporary, "w");
    if(!file) {
        fprintf(stderr, "runtime: cannot write state %s: %s\n", temporary, strerror(errno));
        return false;
    }

    app_persisted_state_t value;
    app_state_export_persisted(runtime->state, &value);
    fprintf(file, "schema=%u\n", STATE_SCHEMA_VERSION);
    fprintf(file, "brightness=%u\n", value.brightness);
    fprintf(file, "volume=%u\n", value.volume);
    fprintf(file, "screensaver_theme=%s\nscreensaver_delay=%u\nscreensaver_shuffle=%u\n",
            value.screensaver.theme_id, value.screensaver.delay_seconds, value.screensaver.shuffle);
    fprintf(file, "screensaver_standby=%s\n", value.screensaver.standby_id);
    fprintf(file, "controls=%u\n", value.control_flags);
    fprintf(file, "gesture_enabled=%u\ngesture_confirm=%u\ngesture_sensitivity=%u\ngesture_mask=%u\n",
            value.gestures.enabled, value.gestures.confirm_sensitive,
            value.gestures.sensitivity, value.gestures.enabled_mask);
    for(size_t i = 0; i < APP_GESTURE_COUNT; ++i)
        fprintf(file, "gesture_action_%u=%u\n", (unsigned)i, value.gestures.actions[i]);
    fprintf(file, "alarms=%u\n", value.alarm_mask);
    fprintf(file, "alarm_count=%u\n", value.alarm_count);
    for(size_t i = 0; i < value.alarm_count; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "alarm_time_%u", (unsigned)i);
        write_hex(file, key, value.alarm_time[i]);
        snprintf(key, sizeof(key), "alarm_label_%u", (unsigned)i);
        write_hex(file, key, value.alarm_label[i]);
        fprintf(file, "alarm_days_%u=%u\n", (unsigned)i, value.alarm_days[i]);
    }
    fprintf(file, "todos=%u\n", value.todo_done_mask);
    fprintf(file, "todo_count=%u\n", value.todo_count);
    for(size_t i = 0; i < value.todo_count && i < APP_TODO_CAPACITY; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "todo_%u", (unsigned)i);
        write_hex(file, key, value.todo_text[i]);
        snprintf(key, sizeof(key), "todo_id_%u", (unsigned)i);
        write_hex(file, key, value.todo_id[i]);
        snprintf(key, sizeof(key), "todo_time_%u", (unsigned)i);
        write_hex(file, key, value.todo_time[i]);
    }
    fprintf(file, "devices=%u\n", value.device_mask);
    fprintf(file, "computer=%u\n", value.computer_state);
    fprintf(file, "pomodoro_rounds=%u\n", value.pomodoro_rounds);
    fprintf(file, "pomodoro_mode=%u\n", value.pomodoro_mode);
    fprintf(file, "pomodoro_seconds=%u\n", value.pomodoro_seconds);
    fprintf(file, "pomodoro_focus_seconds=%u\n", value.pomodoro_focus_seconds);
    fprintf(file, "vocabulary_known_mask=%u\n", value.vocabulary_known_mask);
    fprintf(file, "vocabulary_index=%u\n", value.vocabulary_index);
    fprintf(file, "vocabulary_reviewed=%u\n", value.vocabulary_reviewed);
    fprintf(file, "vocabulary_correct=%u\n", value.vocabulary_correct);
    fprintf(file, "vocabulary_cycles=%u\n", value.vocabulary_cycles);
    bool saved = !ferror(file) && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if(fclose(file) != 0) saved = false;
    if(!saved || rename(temporary, runtime->state_path) != 0) {
        fprintf(stderr, "runtime: cannot commit state %s: %s\n", runtime->state_path, strerror(errno));
        return false;
    }
    char parent[256];
    snprintf(parent, sizeof(parent), "%s", runtime->state_path);
    char *slash = strrchr(parent, '/');
    if(slash) *slash = '\0';
    int directory = open(slash ? (parent[0] ? parent : "/") : ".", O_RDONLY | O_DIRECTORY);
    if(directory < 0) return false;
    saved = fsync(directory) == 0;
    close(directory);
    if(!saved) return false;
    runtime->dirty = false;
    return true;
}

app_runtime_t *app_runtime_create(const char *state_path)
{
    app_runtime_t *runtime = calloc(1, sizeof(*runtime));
    if(!runtime) return NULL;
    runtime->state = app_state_create();
    if(!runtime->state) {
        free(runtime);
        return NULL;
    }
    snprintf(runtime->state_path, sizeof(runtime->state_path), "%s",
             state_path && state_path[0] ? state_path : "/oem/data/binbot_state.ini");
    load_state(runtime);
    app_state_subscribe(runtime->state, state_changed, runtime);
    return runtime;
}

void app_runtime_destroy(app_runtime_t *runtime)
{
    if(!runtime) return;
    app_runtime_flush(runtime);
    app_state_destroy(runtime->state);
    free(runtime);
}

app_state_t *app_runtime_state(app_runtime_t *runtime)
{
    return runtime ? runtime->state : NULL;
}

void app_runtime_tick(app_runtime_t *runtime, uint32_t monotonic_ms)
{
    if(!runtime) return;
    app_state_tick(runtime->state, monotonic_ms);
    if(runtime->dirty) {
        if(runtime->dirty_since_ms == 0) runtime->dirty_since_ms = monotonic_ms;
        if(monotonic_ms - runtime->dirty_since_ms >= 800U) {
            save_state(runtime);
            runtime->dirty_since_ms = 0;
        }
    }
}

bool app_runtime_flush(app_runtime_t *runtime)
{
    return runtime && (!runtime->dirty || save_state(runtime));
}
