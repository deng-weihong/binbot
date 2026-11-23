#include "ui_internal.h"
#include "binbot/ambient_display.h"
#include "binbot/app_weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Only this adapter knows application routes, page suspension, idle policy and
 * business data. Themes depend exclusively on the ambient display library. */
struct ui_screensaver {
    ui_app_t *app;
    binbot_ambient_display_t *display;
    lv_timer_t *timer;
    char last_theme[BINBOT_AMBIENT_ID_SIZE];
};

void ui_screensaver_snapshot(ui_screensaver_t *saver, binbot_ambient_snapshot_t *data)
{
    memset(data, 0, sizeof(*data));
    time_t now = time(NULL);
    data->unix_time = now;
    FILE *uptime = fopen("/proc/uptime", "r");
    double seconds = 0;
    if(uptime) { if(fscanf(uptime, "%lf", &seconds) == 1 && seconds >= 0) data->uptime_seconds = (uint32_t)seconds; fclose(uptime); }
    struct tm local;
    if(localtime_r(&now, &local)) {
        static const char *days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        data->year = local.tm_year + 1900;
        data->month = local.tm_mon + 1;
        data->day = local.tm_mday;
        data->weekday = local.tm_wday;
        data->hour = local.tm_hour;
        data->minute = local.tm_min;
        data->second = local.tm_sec;
        strftime(data->time, sizeof(data->time), "%H:%M", &local);
        snprintf(data->date, sizeof(data->date), "%d月%d日  %s",
                 data->month, data->day, days[local.tm_wday]);
    }
    else snprintf(data->time, sizeof(data->time), "--:--");
    const app_model_t *model = app_state_model(saver->app->state);
    data->todo_total = (unsigned)model->todo_count;
    for(size_t i = 0; i < model->todo_count; ++i) {
        if(i < 5) {
            snprintf(data->todos[i].text, sizeof(data->todos[i].text), "%s", model->todos[i].text);
            snprintf(data->todos[i].time, sizeof(data->todos[i].time), "%s", model->todos[i].time);
            data->todos[i].done = model->todos[i].done;
        }
        if(model->todos[i].done) ++data->todo_done;
        else if(!data->next_todo[0]) snprintf(data->next_todo, sizeof(data->next_todo), "%s", model->todos[i].text);
    }
    data->focus_running = model->pomodoro.running;
    data->focus_seconds = model->pomodoro.seconds_left;
    data->focus_minutes = model->pomodoro.rounds_completed * model->pomodoro.focus_seconds / 60;
    if(model->pomodoro.mode == APP_POMODORO_FOCUS && model->pomodoro.focus_seconds >= model->pomodoro.seconds_left)
        data->focus_minutes += (model->pomodoro.focus_seconds - model->pomodoro.seconds_left) / 60;
    for(size_t i = 0; i < saver->app->calendar.count && data->event_count < 5; ++i) {
        const app_calendar_event_t *event = &saver->app->calendar.events[i];
        if(event->all_day || event->end <= now) continue;
        unsigned j = data->event_count++;
        snprintf(data->events[j].title, sizeof(data->events[j].title), "%s", event->data.summary);
        struct tm start;
        if(localtime_r(&event->start, &start)) strftime(data->events[j].time, sizeof(data->events[j].time), "%H:%M", &start);
        data->events[j].start = event->start;
        data->events[j].end = event->end;
    }
    /* These named zones account for daylight saving without changing process TZ. */
    struct tm utc;
    if(gmtime_r(&now, &utc)) {
        int year = utc.tm_year + 1900;
        struct tm march = {.tm_year = year - 1900, .tm_mon = 2, .tm_mday = 31};
        struct tm october = {.tm_year = year - 1900, .tm_mon = 9, .tm_mday = 31};
        time_t m = timegm(&march), o = timegm(&october);
        struct tm mt, ot; gmtime_r(&m, &mt); gmtime_r(&o, &ot);
        bool london_dst = now >= m - mt.tm_wday * 86400 + 3600 && now < o - ot.tm_wday * 86400 + 3600;
        struct tm us_m = {.tm_year = year - 1900, .tm_mon = 2, .tm_mday = 8};
        struct tm us_n = {.tm_year = year - 1900, .tm_mon = 10, .tm_mday = 1};
        m = timegm(&us_m); o = timegm(&us_n); gmtime_r(&m, &mt); gmtime_r(&o, &ot);
        bool ny_dst = now >= m + ((7 - mt.tm_wday) % 7) * 86400 + 7 * 3600 &&
                      now < o + ((7 - ot.tm_wday) % 7) * 86400 + 6 * 3600;
        int offsets[] = {london_dst ? 1 : 0, ny_dst ? -4 : -5, 9};
        for(unsigned i = 0; i < 3; ++i) {
            time_t shifted = now + offsets[i] * 3600;
            if(gmtime_r(&shifted, &utc)) strftime(data->world_time[i], sizeof(data->world_time[i]), "%H:%M", &utc);
        }
    }
    data->playing = saver->app->music.playing;
    const ui_music_track_t *track = ui_music_current_track(saver->app);
    if(track) {
        snprintf(data->track, sizeof(data->track), "%s", track->title);
        snprintf(data->artist, sizeof(data->artist), "%s", track->artist);
    }
    bb_component_pen_t weather;
    data->weather_valid = saver->app->modules && app_modules_weather(saver->app->modules, &weather);
    if(data->weather_valid) {
        const app_weather_info_t *info = app_weather_info(weather.condition);
        if(!info) data->weather_valid = false;
        else {
            snprintf(data->weather, sizeof(data->weather), "%s", info->name);
            snprintf(data->weather_id, sizeof(data->weather_id), "%s", info->icon);
            snprintf(data->weather_icon, sizeof(data->weather_icon), "home/weather/%s.png", info->icon);
            data->temperature = weather.temperature >= 0 ? (weather.temperature + 5) / 10 : (weather.temperature - 5) / 10;
            data->temperature_low = (weather.low + (weather.low < 0 ? -5 : 5)) / 10;
            data->temperature_high = (weather.high + (weather.high < 0 ? -5 : 5)) / 10;
        }
    }
    const char *city = getenv("BINBOT_WEATHER_CITY");
    if(city && !strcmp(city, "Shenzhen")) city = "深圳";
    if(city) snprintf(data->city, sizeof(data->city), "%s", city);
}

static const lv_font_t *font(void *user_data, lv_obj_t *owner, uint16_t pixels, unsigned weight)
{
    (void)user_data;
    const lv_font_t *font = ui_ref_font(pixels, weight >= 600 ? UI_FONT_SEMIBOLD : weight >= 500 ? UI_FONT_MEDIUM : UI_FONT_REGULAR);
    return ui_resources_retain_font(owner, font) ? font : LV_FONT_DEFAULT;
}

bool ui_screensaver_active(const ui_screensaver_t *saver)
{
    return saver && binbot_ambient_active(saver->display);
}

binbot_ambient_display_t *ui_screensaver_display(ui_screensaver_t *saver)
{
    return saver ? saver->display : NULL;
}

void ui_screensaver_raise(ui_screensaver_t *saver)
{
    if(saver) binbot_ambient_raise(saver->display);
}

void ui_screensaver_wake(ui_screensaver_t *saver)
{
    if(!saver) return;
    lv_display_trigger_activity(NULL);
    if(!ui_screensaver_active(saver)) return;
    binbot_ambient_hide(saver->display);
    if(saver->app->current.set_paused)
        saver->app->current.set_paused(saver->app->current.context, false);
}

static void wake(void *user_data) { ui_screensaver_wake(user_data); }

static bool show(ui_screensaver_t *saver, const char *theme_id)
{
    if(!saver || saver->app->route_pending || ui_gesture_popup_active(saver->app->gestures)) return false;
    bool active = ui_screensaver_active(saver);
    ui_app_close_control_center(saver->app);
    if(!active && saver->app->current.set_paused)
        saver->app->current.set_paused(saver->app->current.context, true);
    binbot_ambient_snapshot_t data;
    ui_screensaver_snapshot(saver, &data);
    if(!binbot_ambient_show(saver->display, theme_id, &data)) {
        if(saver->app->current.set_paused)
            saver->app->current.set_paused(saver->app->current.context, false);
        ui_app_show_toast(saver->app, "屏保加载失败");
        return false;
    }
    snprintf(saver->last_theme, sizeof(saver->last_theme), "%s",
             binbot_ambient_current_theme(saver->display));
    return true;
}

bool ui_screensaver_preview(ui_screensaver_t *saver, const char *theme_id)
{
    return show(saver, theme_id) &&
           !strcmp(binbot_ambient_current_theme(saver->display), theme_id);
}

void ui_screensaver_show(ui_screensaver_t *saver)
{
    if(!saver || ui_screensaver_active(saver)) return;
    const app_screensaver_settings_t *settings = &app_state_model(saver->app->state)->screensaver;
    const char *id = settings->theme_id;
    size_t count = binbot_ambient_theme_count(saver->display);
    if(settings->shuffle && strcmp(id, "face") && count > 2) {
        size_t previous = count;
        for(size_t i = 0; i < count; ++i)
            if(!strcmp(binbot_ambient_theme_at(saver->display, i)->id, saver->last_theme)) { previous = i; break; }
        size_t pick = 1 + (size_t)lv_rand(0, (uint32_t)(count - (previous > 0 && previous < count ? 3 : 2)));
        if(previous > 0 && previous < count && pick >= previous) ++pick;
        id = binbot_ambient_theme_at(saver->display, pick)->id;
    }
    (void)show(saver, id);
}

static void tick(lv_timer_t *timer)
{
    ui_screensaver_t *saver = lv_timer_get_user_data(timer);
    if(ui_screensaver_active(saver)) {
        binbot_ambient_snapshot_t data;
        ui_screensaver_snapshot(saver, &data);
        binbot_ambient_update(saver->display, &data);
        return;
    }
    ui_app_t *app = saver->app;
    uint32_t delay = app_state_model(app->state)->screensaver.delay_seconds;
    if(delay && !app->route_pending && !app->content_frozen &&
       app->route != UI_ROUTE_DIAGNOSTICS &&
       app->voice_state != BINBOT_DIALOG_STATE_LISTENING &&
       app->voice_state != BINBOT_DIALOG_STATE_THINKING &&
       app->voice_state != BINBOT_DIALOG_STATE_SPEAKING &&
       lv_display_get_inactive_time(NULL) >= delay * 1000U) ui_screensaver_show(saver);
}

static void selected(void *user_data, const char *id)
{
    ui_screensaver_t *saver = user_data;
    app_screensaver_settings_t settings = app_state_model(saver->app->state)->screensaver;
    snprintf(settings.theme_id, sizeof(settings.theme_id), "%s", id);
    snprintf(settings.standby_id, sizeof(settings.standby_id), "%s", id);
    app_state_set_screensaver(saver->app->state, &settings);
    snprintf(saver->last_theme, sizeof(saver->last_theme), "%s", id);
}

ui_screensaver_t *ui_screensaver_create(ui_app_t *app)
{
    ui_screensaver_t *saver = calloc(1, sizeof(*saver));
    if(!saver) return NULL;
    saver->app = app;
    binbot_ambient_config_t config = {
        .parent = app->root, .asset_root = getenv("BINBOT_ASSET_ROOT"),
        .font = font, .wake = wake, .selected = selected, .user_data = saver,
    };
    saver->display = binbot_ambient_create(&config);
    if(!saver->display) { free(saver); return NULL; }
    saver->timer = lv_timer_create(tick, 250, saver);
    if(!saver->timer) { binbot_ambient_destroy(saver->display); free(saver); return NULL; }
    lv_display_trigger_activity(NULL);
    return saver;
}

void ui_screensaver_destroy(ui_screensaver_t *saver)
{
    if(!saver) return;
    lv_timer_delete(saver->timer);
    binbot_ambient_destroy(saver->display);
    free(saver);
}

