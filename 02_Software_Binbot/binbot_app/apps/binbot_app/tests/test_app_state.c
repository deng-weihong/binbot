#include "binbot/app_state.h"
#include "binbot/app_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t observed_changes;

static void observe_changes(uint32_t changes, const app_model_t *model, void *user_data)
{
    (void)model;
    (void)user_data;
    observed_changes = changes;
}

static void test_pomodoro_progress_and_phase(void)
{
    app_state_t *state = app_state_create();
    assert(state != NULL);
    assert(app_state_subscribe(state, observe_changes, NULL));

    const app_model_t *model = app_state_model(state);
    assert(model->pomodoro.mode == APP_POMODORO_FOCUS);
    assert(model->pomodoro.seconds_left == APP_POMODORO_FOCUS_SECONDS);
    assert(model->pomodoro.rounds_completed == 0);

    app_state_start_pomodoro(state);
    assert(app_state_model(state)->pomodoro.running);
    app_state_tick(state, 1000U);
    app_state_tick(state, 2000U);
    assert(app_state_model(state)->pomodoro.seconds_left ==
           APP_POMODORO_FOCUS_SECONDS - 1U);
    assert((observed_changes & APP_CHANGE_POMODORO_PROGRESS) != 0);

    app_persisted_state_t persisted = {0};
    persisted.brightness = 72;
    persisted.volume = 45;
    persisted.pomodoro_mode = APP_POMODORO_FOCUS;
    persisted.pomodoro_seconds = 1;
    app_state_import_persisted(state, &persisted);
    app_state_start_pomodoro(state);
    app_state_tick(state, 10000U);
    app_state_tick(state, 11000U);
    model = app_state_model(state);
    assert(model->pomodoro.mode == APP_POMODORO_BREAK);
    assert(model->pomodoro.seconds_left == APP_POMODORO_BREAK_SECONDS);
    assert(!model->pomodoro.running);
    assert(model->pomodoro.rounds_completed == 1);
    assert((observed_changes & APP_CHANGE_POMODORO_PHASE) != 0);

    app_state_start_pomodoro(state);
    app_state_tick(state, 12000U);
    app_state_tick(state, 13000U);
    app_state_pause_pomodoro(state);
    assert(!app_state_model(state)->pomodoro.running);
    assert(app_state_model(state)->pomodoro.seconds_left ==
           APP_POMODORO_BREAK_SECONDS - 1U);
    app_state_destroy(state);
}

static void test_brightness_minimum(void)
{
    app_state_t *state = app_state_create();
    assert(state != NULL);

    app_state_set_brightness(state, 0);
    assert(app_state_model(state)->brightness == APP_BRIGHTNESS_MIN);
    app_state_set_brightness(state, APP_BRIGHTNESS_MAX);
    assert(app_state_model(state)->brightness == APP_BRIGHTNESS_MAX);

    app_persisted_state_t persisted = {0};
    persisted.brightness = 0;
    app_state_import_persisted(state, &persisted);
    assert(app_state_model(state)->brightness == APP_BRIGHTNESS_MIN);
    app_state_destroy(state);
}

static void test_vocabulary_review_and_persistence(void)
{
    app_state_t *state = app_state_create();
    assert(state != NULL);
    const app_model_t *model = app_state_model(state);
    assert(model->vocabulary.card_count == APP_VOCABULARY_CAPACITY);
    assert(model->vocabulary.current_index == 0);

    for(size_t i = 0; i < APP_VOCABULARY_CAPACITY; ++i) {
        bool complete = app_state_vocabulary_answer(
            state, APP_VOCABULARY_ANSWER_KNOWN);
        if(i + 1U < APP_VOCABULARY_CAPACITY) assert(!complete);
        else assert(complete);
    }
    model = app_state_model(state);
    assert(model->vocabulary.complete);
    assert(model->vocabulary.known_mask == ((1U << APP_VOCABULARY_CAPACITY) - 1U));
    assert(model->vocabulary.cycles_completed == 1);

    /* Answering after completion starts a fresh round at the first card. */
    assert(!app_state_vocabulary_answer(state, APP_VOCABULARY_ANSWER_AGAIN));
    model = app_state_model(state);
    assert(!model->vocabulary.complete);
    assert(model->vocabulary.current_index == 1);
    assert(model->vocabulary.known_mask == 0);

    app_persisted_state_t persisted = {0};
    app_state_export_persisted(state, &persisted);
    app_state_t *restored = app_state_create();
    assert(restored != NULL);
    app_state_import_persisted(restored, &persisted);
    model = app_state_model(restored);
    assert(model->vocabulary.current_index == persisted.vocabulary_index);
    assert(model->vocabulary.known_mask == persisted.vocabulary_known_mask);
    assert(model->vocabulary.reviewed_count == persisted.vocabulary_reviewed);

    app_state_destroy(restored);
    app_state_destroy(state);
}

static void test_remote_todo_crud_and_persistence(void)
{
    app_state_t *state = app_state_create();
    assert(state != NULL);
    assert(app_state_add_todo(state, "后台新增待办"));
    const size_t index = app_state_model(state)->todo_count - 1U;
    assert(app_state_set_todo_done(state, index, true));
    assert(app_state_edit_todo(state, index, "后台编辑待办"));

    app_persisted_state_t persisted = {0};
    app_state_export_persisted(state, &persisted);
    app_state_t *restored = app_state_create();
    assert(restored != NULL);
    app_state_import_persisted(restored, &persisted);
    const app_model_t *model = app_state_model(restored);
    assert(model->todo_count == persisted.todo_count);
    assert(strcmp(model->todos[index].text, "后台编辑待办") == 0);
    assert(model->todos[index].done);
    assert(app_state_delete_todo(restored, index));
    assert(app_state_model(restored)->todo_count == persisted.todo_count - 1U);
    assert(!app_state_delete_todo(restored, APP_TODO_CAPACITY));

    app_state_destroy(restored);
    app_state_destroy(state);
}

static void test_todo_time_and_persistence(void)
{
    app_state_t *state = app_state_create();
    assert(state);
    size_t initial_count = app_state_model(state)->todo_count;
    assert(!app_state_add_todo_at_time(state, "无效时间", "24:00"));
    assert(!app_state_add_todo_at_time(state, "无效时间", "18:60"));
    assert(!app_state_add_todo_at_time(state, "无效时间", "8:00"));
    assert(!app_state_add_todo_at_time(state, "无效时间", "aa:bb"));
    assert(app_state_model(state)->todo_count == initial_count);
    assert(app_state_add_todo_at_time(state, "喝水休息", "16:00"));
    app_persisted_state_t saved = {0};
    app_state_export_persisted(state, &saved);
    app_state_t *restored = app_state_create();
    app_state_import_persisted(restored, &saved);
    assert(strcmp(app_state_model(restored)->todos[initial_count].time, "16:00") == 0);
    assert(app_state_delete_todo(restored, 0));
    assert(strcmp(app_state_model(restored)->todos[initial_count - 1].time, "16:00") == 0);
    saved.todo_time[initial_count][0] = '9';
    app_state_import_persisted(restored, &saved);
    assert(app_state_model(restored)->todos[initial_count].time[0] == '\0');
    app_state_destroy(restored);
    app_state_destroy(state);
}

static void test_alarm_editing_and_persistence(void)
{
    app_state_t *state = app_state_create();
    assert(state);
    size_t count = app_state_model(state)->alarm_count;
    assert(!app_state_save_alarm(state, count, "24:00", "起床", 0x1f, true));
    assert(!app_state_save_alarm(state, count, "07:60", "起床", 0x1f, true));
    assert(!app_state_save_alarm(state, count, "", "起床", 0x1f, true));
    assert(!app_state_save_alarm(state, count, "07:30", "", 0x1f, true));
    assert(!app_state_save_alarm(state, count, "07:30", "起床", 0x80, true));
    assert(app_state_model(state)->alarm_count == count);
    assert(app_state_save_alarm(state, count, "07:30", "起床", 0x60, true));
    assert(strcmp(app_state_model(state)->alarms[count].repeat, "周末") == 0);
    assert(app_state_save_alarm(state, count, "08:15", "运动", 0x05, false));
    assert(strcmp(app_state_model(state)->alarms[count].repeat, "周一 周三") == 0);
    app_persisted_state_t saved = {0};
    app_state_export_persisted(state, &saved);
    app_state_t *restored = app_state_create();
    app_state_import_persisted(restored, &saved);
    const app_model_t *model = app_state_model(restored);
    assert(model->alarm_count == count + 1);
    assert(strcmp(model->alarms[count].time, "08:15") == 0);
    assert(strcmp(model->alarms[count].label, "运动") == 0);
    assert(model->alarms[count].days == 5 && !model->alarms[count].enabled);
    assert(app_state_delete_alarm(restored, 0));
    assert(strcmp(model->alarms[count - 1].time, "08:15") == 0);
    while(model->alarm_count < APP_ALARM_CAPACITY)
        assert(app_state_save_alarm(restored, model->alarm_count, "12:00", "午休", 0, true));
    assert(!app_state_save_alarm(restored, APP_ALARM_CAPACITY, "12:00", "午休", 0, true));
    assert(!app_state_delete_alarm(restored, APP_ALARM_CAPACITY));
    while(model->alarm_count) assert(app_state_delete_alarm(restored, 0));
    app_state_export_persisted(restored, &saved);
    app_state_import_persisted(state, &saved);
    assert(app_state_model(state)->alarm_count == 0);
    app_state_destroy(restored);
    app_state_destroy(state);
}

static void test_custom_pomodoro_duration(void)
{
    app_state_t *state = app_state_create();
    const app_model_t *model = app_state_model(state);
    app_state_set_pomodoro_duration(state, 45);
    assert(model->pomodoro.focus_seconds == 2700 && model->pomodoro.seconds_left == 2700);
    app_state_set_pomodoro_duration(state, 4);
    app_state_set_pomodoro_duration(state, 121);
    assert(model->pomodoro.focus_seconds == 2700);
    app_state_start_pomodoro(state);
    app_state_tick(state, 1000);
    app_state_tick(state, 3000);
    assert(model->pomodoro.seconds_left == 2698);
    app_state_reset_pomodoro(state);
    assert(model->pomodoro.seconds_left == 2700 && !model->pomodoro.running);
    app_state_set_pomodoro_mode(state, APP_POMODORO_BREAK);
    app_state_start_pomodoro(state);
    app_state_tick(state, 10000);
    app_state_tick(state, 310000);
    assert(model->pomodoro.mode == APP_POMODORO_FOCUS);
    assert(model->pomodoro.seconds_left == 2700 && !model->pomodoro.running);
    app_state_destroy(state);
}

static void test_runtime_alarm_migration_and_roundtrip(void)
{
    char path[] = "/tmp/binbot-state-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("schema=3\nbrightness=72\nvolume=45\nalarms=5\npomodoro_seconds=1200\n", file) >= 0);
    assert(fclose(file) == 0);
    app_runtime_t *runtime = app_runtime_create(path);
    assert(runtime);
    app_state_t *state = app_runtime_state(runtime);
    const app_model_t *model = app_state_model(state);
    assert(model->alarm_count == APP_ALARM_COUNT);
    assert(model->alarms[0].enabled && !model->alarms[1].enabled);
    assert(model->alarms[0].days == 0x1f);
    assert(model->pomodoro.focus_seconds == 1500 && model->pomodoro.seconds_left == 1200);
    app_state_set_pomodoro_duration(state, 60);
    assert(app_state_save_alarm(state, model->alarm_count, "18:25", "运动", 0x55, true));
    app_runtime_flush(runtime);
    app_runtime_destroy(runtime);
    runtime = app_runtime_create(path);
    assert(runtime);
    model = app_state_model(app_runtime_state(runtime));
    assert(model->alarm_count == APP_ALARM_COUNT + 1);
    assert(strcmp(model->alarms[APP_ALARM_COUNT].time, "18:25") == 0);
    assert(strcmp(model->alarms[APP_ALARM_COUNT].label, "运动") == 0);
    assert(model->alarms[APP_ALARM_COUNT].days == 0x55);
    assert(model->alarms[APP_ALARM_COUNT].enabled);
    assert(model->pomodoro.focus_seconds == 3600 && model->pomodoro.seconds_left == 3600);
    app_runtime_destroy(runtime);
    unlink(path);
}

static void test_desk_independent_devices_and_sleep(void)
{
    app_state_t *state = app_state_create();
    assert(state);
    const app_model_t *model = app_state_model(state);
    app_state_set_device(state, 2, true);
    assert(model->devices[2].enabled);
    assert(!model->devices[3].enabled);
    app_state_set_device(state, 0, false);
    assert(!model->devices[0].enabled && model->devices[1].enabled);
    app_state_select_computer(state, 100);
    app_state_confirm_computer_wake(state, 100);
    assert(model->computer == APP_COMPUTER_WAKING);
    app_state_sleep_computer(state);
    app_state_tick(state, 5000);
    assert(model->computer == APP_COMPUTER_SLEEP);
    app_persisted_state_t saved;
    app_state_export_persisted(state, &saved);
    app_state_t *restored = app_state_create();
    app_state_import_persisted(restored, &saved);
    const app_model_t *copy = app_state_model(restored);
    assert(!copy->devices[0].enabled && copy->devices[1].enabled);
    assert(copy->devices[2].enabled && !copy->devices[3].enabled);
    assert(copy->computer == APP_COMPUTER_SLEEP);
    app_state_destroy(restored);
    app_state_destroy(state);
}

static void test_screensaver_migration_and_roundtrip(void)
{
    char path[] = "/tmp/binbot-saver-state-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "w"); assert(file);
    assert(fputs("schema=5\nbrightness=72\nvolume=45\n", file) >= 0);
    assert(!fclose(file));
    app_runtime_t *runtime = app_runtime_create(path); assert(runtime);
    app_state_t *state = app_runtime_state(runtime);
    const app_screensaver_settings_t *s = &app_state_model(state)->screensaver;
    assert(!strcmp(s->theme_id, "face") && s->delay_seconds == 60 && !s->shuffle);
    app_screensaver_settings_t settings = {.theme_id = "face", .standby_id = "shapes", .delay_seconds = 0, .shuffle = true};
    assert(app_state_set_screensaver(state, &settings));
    settings.delay_seconds = 3601;
    assert(!app_state_set_screensaver(state, &settings));
    settings.delay_seconds = 30;
    strcpy(settings.theme_id, "../invalid");
    assert(!app_state_set_screensaver(state, &settings));
    memset(settings.theme_id, 'a', sizeof(settings.theme_id));
    assert(!app_state_set_screensaver(state, &settings));
    app_runtime_flush(runtime); app_runtime_destroy(runtime);
    runtime = app_runtime_create(path); assert(runtime);
    s = &app_state_model(app_runtime_state(runtime))->screensaver;
    assert(!strcmp(s->theme_id, "face") && !strcmp(s->standby_id, "shapes") && s->delay_seconds == 0 && s->shuffle);
    app_runtime_destroy(runtime);
    file = fopen(path, "w"); assert(file);
    assert(fputs("schema=6\nscreensaver_theme=../bad\nscreensaver_delay=30\n", file) >= 0);
    assert(!fclose(file));
    runtime = app_runtime_create(path); assert(runtime);
    s = &app_state_model(app_runtime_state(runtime))->screensaver;
    assert(!strcmp(s->theme_id, "face") && s->delay_seconds == 60);
    app_runtime_destroy(runtime);
    file = fopen(path, "w"); assert(file);
    assert(fputs("schema=6\nscreensaver_theme=terminal\nscreensaver_delay=60\nscreensaver_shuffle=0\n", file) >= 0);
    assert(!fclose(file));
    runtime = app_runtime_create(path); assert(runtime);
    s = &app_state_model(app_runtime_state(runtime))->screensaver;
    assert(!strcmp(s->theme_id, "ban-shell") && !strcmp(s->standby_id, "ban-shell"));
    assert(s->delay_seconds == 60 && !s->shuffle);
    app_runtime_flush(runtime);app_runtime_destroy(runtime);
    runtime = app_runtime_create(path);assert(runtime);
    assert(!strcmp(app_state_model(app_runtime_state(runtime))->screensaver.theme_id, "ban-shell"));
    app_runtime_destroy(runtime);
    const char *old_ids[]={"green-screen","orbit-deck","warm-radio","amber-clock"};
    const char *new_ids[]={"ban-shell","circuit-core","dial-radio","nixie-tubes"};
    for(unsigned i=0;i<4;++i){
        file=fopen(path,"w");assert(file);
        fprintf(file,"schema=6\nscreensaver_theme=face\nscreensaver_standby=%s\nscreensaver_delay=300\nscreensaver_shuffle=1\n",old_ids[i]);fclose(file);
        runtime=app_runtime_create(path);assert(runtime);s=&app_state_model(app_runtime_state(runtime))->screensaver;
        assert(!strcmp(s->theme_id,"face")&&!strcmp(s->standby_id,new_ids[i]));
        assert(s->delay_seconds==300&&s->shuffle);
        app_runtime_flush(runtime);app_runtime_destroy(runtime);
        runtime=app_runtime_create(path);assert(runtime);
        assert(!strcmp(app_state_model(app_runtime_state(runtime))->screensaver.standby_id,new_ids[i]));
        app_runtime_destroy(runtime);
    }
    unlink(path);
}

int main(void)
{
    test_brightness_minimum();
    test_pomodoro_progress_and_phase();
    test_vocabulary_review_and_persistence();
    test_remote_todo_crud_and_persistence();
    test_todo_time_and_persistence();
    test_alarm_editing_and_persistence();
    test_custom_pomodoro_duration();
    test_runtime_alarm_migration_and_roundtrip();
    test_desk_independent_devices_and_sleep();
    test_screensaver_migration_and_roundtrip();
    return 0;
}
