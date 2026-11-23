#include "ui_internal.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ui_music_track_t music_tracks[UI_MUSIC_TRACK_COUNT] = {
    {"稻香", "周杰伦 · 魔杰座", 224U, "/oem/assets/music/1.mp3"},
    {"窗边的蓝", "慢慢听 · 桌面电台", 198U, NULL},
    {"回到此刻", "慢慢听 · 桌面电台", 246U, NULL},
};

/* Every route owns one live screen tree.  Route changes are applied on the
 * next LVGL turn so event callbacks never delete the object that dispatched
 * them. */
static void destroy_screen(ui_screen_t *screen)
{
    if(!screen) return;
    if(screen->destroy && screen->context) screen->destroy(screen->context);
    if(screen->root && lv_obj_is_valid(screen->root)) lv_obj_delete(screen->root);
    *screen = (ui_screen_t){0};
}

static void layout_page_layer(ui_app_t *app);
static void enable_event_bubble(lv_obj_t *object);
static void set_content_frozen(ui_app_t *app, bool frozen);
static void music_notify(ui_app_t *app);

static void release_control_center(void *user_data)
{
    ui_app_t *app = user_data;
    if(!app->control_center || ui_control_center_is_active(app->control_center)) return;
    ui_control_center_t *center = app->control_center;
    app->control_center = NULL;
    ui_control_center_destroy(center);
    lv_image_cache_drop(NULL);
    ui_resources_collect();
}

static ui_screen_t create_screen(ui_app_t *app, ui_route_t route)
{
    switch(route) {
        case UI_ROUTE_APPS: return ui_apps_screen_create(app);
        case UI_ROUTE_MUSIC: return ui_music_screen_create(app);
        case UI_ROUTE_NOISE: return ui_noise_screen_create(app);
        case UI_ROUTE_ALARM: return ui_alarm_screen_create(app);
        case UI_ROUTE_POMODORO: return ui_pomodoro_screen_create(app);
        case UI_ROUTE_VOCABULARY: return ui_vocabulary_screen_create(app);
        case UI_ROUTE_TODO: return ui_todo_screen_create(app);
        case UI_ROUTE_CALENDAR: return ui_calendar_screen_create(app);
        case UI_ROUTE_CALENDAR_DETAIL: return ui_calendar_detail_screen_create(app);
        case UI_ROUTE_DEVICES: return ui_devices_screen_create(app);
        case UI_ROUTE_ASSISTANT: return ui_assistant_screen_create(app);
        case UI_ROUTE_CAMERA: return ui_camera_screen_create(app);
        case UI_ROUTE_SETTINGS: return ui_settings_screen_create(app);
        case UI_ROUTE_SCREENSAVER_SETTINGS: return ui_screensaver_settings_screen_create(app);
        case UI_ROUTE_WIFI: return ui_wifi_screen_create(app);
        case UI_ROUTE_BLUETOOTH: return ui_bluetooth_screen_create(app);
        case UI_ROUTE_LUNCH: return ui_lunch_screen_create(app);
        case UI_ROUTE_MODULES: return ui_modules_screen_create(app);
        case UI_ROUTE_ABOUT: return ui_about_screen_create(app);
        case UI_ROUTE_DIAGNOSTICS: return ui_diagnostics_screen_create(app);
        case UI_ROUTE_GESTURES: return ui_gestures_screen_create(app);
        case UI_ROUTE_HOME:
        default: return ui_home_screen_create(app);
    }
}

static bool activate_route(ui_app_t *app, ui_route_t route)
{
    if(!app) return false;
    ui_gesture_before_route(app->gestures, route == UI_ROUTE_CAMERA || app->route == UI_ROUTE_CAMERA);
    lv_async_call_cancel(release_control_center, app);
    if(app->control_center) {
        ui_control_center_destroy(app->control_center);
        app->control_center = NULL;
    }
    destroy_screen(&app->current);
    /* A route owns its image working set. Decode the next page on demand. */
    lv_image_cache_drop(NULL);
    ui_resources_collect();
    app->route = route;
    layout_page_layer(app);
    app->current = create_screen(app, route);
    if(!app->current.root) {
        fprintf(stderr, "ui: failed to create route %d\n", (int)route);
        return false;
    }
    if(app->current.update) {
        app->current.update(app->current.context, app_state_model(app->state), APP_CHANGE_ALL);
    }
    enable_event_bubble(app->root);
    ui_gesture_popup_raise(app->gestures);
    return true;
}

static void layout_page_layer(ui_app_t *app)
{
    if(!app || !app->page_layer || !app->status_bar) return;
    const bool edge_to_edge = app->route == UI_ROUTE_HOME ||
                              app->route == UI_ROUTE_APPS ||
                              app->route == UI_ROUTE_MUSIC ||
                              app->route == UI_ROUTE_NOISE ||
                              app->route == UI_ROUTE_SETTINGS ||
                              app->route == UI_ROUTE_SCREENSAVER_SETTINGS ||
                              app->route == UI_ROUTE_WIFI ||
                              app->route == UI_ROUTE_BLUETOOTH ||
                              app->route == UI_ROUTE_ALARM ||
                              app->route == UI_ROUTE_POMODORO ||
                              app->route == UI_ROUTE_ASSISTANT ||
                              app->route == UI_ROUTE_TODO ||
                              app->route == UI_ROUTE_CALENDAR ||
                              app->route == UI_ROUTE_CALENDAR_DETAIL ||
                              app->route == UI_ROUTE_VOCABULARY ||
                              app->route == UI_ROUTE_CAMERA ||
                              app->route == UI_ROUTE_DEVICES ||
                              app->route == UI_ROUTE_LUNCH ||
                              app->route == UI_ROUTE_MODULES ||
                              app->route == UI_ROUTE_ABOUT ||
                              app->route == UI_ROUTE_DIAGNOSTICS ||
                              app->route == UI_ROUTE_GESTURES;
    if(edge_to_edge) {
        lv_obj_add_flag(app->status_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(app->page_layer, 0, 0);
        lv_obj_set_size(app->page_layer, UI_WIDTH, UI_HEIGHT);
    }
    else {
        lv_obj_remove_flag(app->status_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(app->page_layer, 0, UI_STATUS_HEIGHT);
        lv_obj_set_size(app->page_layer, UI_WIDTH, UI_PAGE_HEIGHT);
    }
}

static ui_route_t initial_route_from_environment(void)
{
    const char *route = getenv("BINBOT_INITIAL_ROUTE");
    if(!route || !route[0] || strcmp(route, "home") == 0) return UI_ROUTE_HOME;
    if(strcmp(route, "apps") == 0) return UI_ROUTE_APPS;
    if(strcmp(route, "music") == 0 || strcmp(route, "player") == 0) return UI_ROUTE_MUSIC;
    if(strcmp(route, "noise") == 0) return UI_ROUTE_NOISE;
    if(strcmp(route, "alarm") == 0) return UI_ROUTE_ALARM;
    if(strcmp(route, "pomodoro") == 0) return UI_ROUTE_POMODORO;
    if(strcmp(route, "vocabulary") == 0) return UI_ROUTE_VOCABULARY;
    if(strcmp(route, "todo") == 0) return UI_ROUTE_TODO;
    if(strcmp(route, "calendar") == 0 || strcmp(route, "cal") == 0) return UI_ROUTE_CALENDAR;
    if(strcmp(route, "devices") == 0) return UI_ROUTE_DEVICES;
    if(strcmp(route, "assistant") == 0) return UI_ROUTE_ASSISTANT;
    if(strcmp(route, "camera") == 0) return UI_ROUTE_CAMERA;
    if(strcmp(route, "settings") == 0) return UI_ROUTE_SETTINGS;
    if(strcmp(route, "screensaver-settings") == 0) return UI_ROUTE_SCREENSAVER_SETTINGS;
    if(strcmp(route, "wifi") == 0) return UI_ROUTE_WIFI;
    if(strcmp(route, "bluetooth") == 0) return UI_ROUTE_BLUETOOTH;
    if(strcmp(route, "lunch") == 0) return UI_ROUTE_LUNCH;
    if(strcmp(route, "modules") == 0) return UI_ROUTE_MODULES;
    if(strcmp(route, "about") == 0) return UI_ROUTE_ABOUT;
    if(strcmp(route, "diag") == 0) return UI_ROUTE_DIAGNOSTICS;
    if(strcmp(route, "gesture") == 0) return UI_ROUTE_GESTURES;
    if(strcmp(route, "screensaver") == 0) return UI_ROUTE_HOME;
    fprintf(stderr, "ui: ignoring unknown BINBOT_INITIAL_ROUTE=%s\n", route);
    return UI_ROUTE_HOME;
}

static void apply_route_async(void *user_data)
{
    ui_app_t *app = user_data;
    if(!app || !app->route_pending) return;

    app->route_pending = false;
    const ui_route_t target = app->pending_route;
    const ui_route_t previous = app->route;
    const bool is_back = app->pending_route_is_back;
    const size_t back_history_len = app->pending_back_history_len;
    app->pending_route_is_back = false;
    app->pending_back_history_len = 0;
    if(!activate_route(app, target)) {
        if(target != UI_ROUTE_HOME) {
            if(activate_route(app, UI_ROUTE_HOME) && previous != UI_ROUTE_HOME) {
                if(app->route_history_len == UI_ROUTE_HISTORY_MAX) {
                    memmove(app->route_history, app->route_history + 1,
                            (UI_ROUTE_HISTORY_MAX - 1) * sizeof(app->route_history[0]));
                    --app->route_history_len;
                }
                app->route_history[app->route_history_len++] = previous;
            }
        }
        return;
    }

    if(target == previous) return;
    if(is_back) {
        /* A back operation targets the top history entry.  Only remove it
         * after the destination screen was created successfully. */
        if(back_history_len < app->route_history_len)
            app->route_history_len = back_history_len;
        if(app->route_history_len > 0 &&
           app->route_history[app->route_history_len - 1] == target) {
            --app->route_history_len;
        }
        return;
    }

    /* A queued route is the only point at which a transition is committed.
     * This prevents repeated clicks while LVGL is between turns from adding
     * several copies of the same current route. */
    if(app->route_history_len == 0 ||
       app->route_history[app->route_history_len - 1] != previous) {
        if(app->route_history_len == UI_ROUTE_HISTORY_MAX) {
            memmove(app->route_history, app->route_history + 1,
                    (UI_ROUTE_HISTORY_MAX - 1) * sizeof(app->route_history[0]));
            --app->route_history_len;
        }
        app->route_history[app->route_history_len++] = previous;
    }
}

static void control_center_clicked(lv_event_t *event)
{
    ui_app_open_control_center(lv_event_get_user_data(event));
}

enum {
    /* A swipe must begin in the display's top edge.  The extra few pixels make
     * the gesture reliable on a resistive panel without stealing normal page
     * gestures that start lower down. */
    CONTROL_CENTER_EDGE_HEIGHT = 36,
    CONTROL_CENTER_DRAG_TRIGGER = 8,
};

/* A gesture can begin on any child object.  LVGL only bubbles events through
 * objects carrying EVENT_BUBBLE, so mark the current screen tree once it has
 * been built and let the app root observe the pointer lifecycle. */
static void enable_event_bubble(lv_obj_t *object)
{
    if(!object) return;
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    uint32_t child_count = lv_obj_get_child_count(object);
    for(uint32_t i = 0; i < child_count; ++i) {
        enable_event_bubble(lv_obj_get_child(object, (int32_t)i));
    }
}

/* A gesture is recognized after LVGL has already pressed the original hit
 * target.  Clear that state so a downward drag cannot leave a button looking
 * pressed while the panel follows the finger. */
static void clear_gesture_pressed_state(void)
{
    lv_obj_t *object = lv_indev_get_active_obj();
    while(object) {
        lv_obj_remove_state(object, LV_STATE_PRESSED);
        object = lv_obj_get_parent(object);
    }
}

static void reset_edge_swipe(ui_app_t *app)
{
    if(!app) return;
    app->control_center_indev = NULL;
    app->control_center_edge_tracking = false;
    app->control_center_dragging = false;
    app->control_center_start_x = 0;
    app->control_center_start_y = 0;
}

static bool edge_swipe_event_indev(ui_app_t *app, lv_event_t *event,
                                   lv_indev_t **indev_out, lv_point_t *point_out)
{
    if(!app || !event || !indev_out || !point_out) return false;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return false;
    lv_indev_get_point(indev, point_out);
    *indev_out = indev;
    return true;
}

static void app_edge_pressed(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    lv_indev_t *indev = NULL;
    lv_point_t point;
    reset_edge_swipe(app);
    if(!edge_swipe_event_indev(app, event, &indev, &point) ||
       ui_screensaver_active(app->screensaver) ||
       ui_gesture_popup_active(app->gestures) ||
       app->route == UI_ROUTE_DIAGNOSTICS ||
       app->route_pending ||
       (app->control_center && ui_control_center_is_active(app->control_center)) ||
       point.y > CONTROL_CENTER_EDGE_HEIGHT) {
        return;
    }
    app->control_center_indev = indev;
    app->control_center_edge_tracking = true;
    app->control_center_start_x = point.x;
    app->control_center_start_y = point.y;
}

static int32_t edge_swipe_panel_distance(const ui_app_t *app, int32_t distance)
{
    /* Map the remaining touch surface to the panel's full off-screen travel,
     * so its final top margin is reached before the finger is released. */
    int32_t available = UI_HEIGHT - 1 - app->control_center_start_y;
    if(distance <= 0 || available <= 0) return 0;
    if(distance >= available) return UI_CONTROL_CENTER_DRAG_MAX;
    return distance * UI_CONTROL_CENTER_DRAG_MAX / available;
}

static void app_edge_pressing(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    lv_indev_t *indev = NULL;
    lv_point_t point;
    if(!app || !app->control_center_edge_tracking ||
       !edge_swipe_event_indev(app, event, &indev, &point) ||
       indev != app->control_center_indev) {
        return;
    }

    const int32_t distance = point.y - app->control_center_start_y;
    if(!app->control_center_dragging) {
        /* An upward or strongly horizontal motion cancels this edge press;
         * it must not turn into a control-center opening later. */
        const int32_t horizontal = abs(point.x - app->control_center_start_x);
        if(distance < 0 && -distance >= CONTROL_CENTER_DRAG_TRIGGER) {
            reset_edge_swipe(app);
            return;
        }
        if(horizontal > distance && horizontal >= CONTROL_CENTER_DRAG_TRIGGER) {
            reset_edge_swipe(app);
            return;
        }
        if(distance < CONTROL_CENTER_DRAG_TRIGGER) return;

        clear_gesture_pressed_state();
        set_content_frozen(app, true);
        if(!ui_app_open_control_center_drag(app, edge_swipe_panel_distance(app, distance))) {
            set_content_frozen(app, false);
            reset_edge_swipe(app);
            return;
        }
        app->control_center_dragging = true;
    }

    if(app->control_center_dragging && app->control_center) {
        ui_app_update_control_center_drag(app, edge_swipe_panel_distance(app, distance));
    }
}

static void app_edge_released(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    lv_indev_t *indev = NULL;
    lv_point_t point;
    if(!app || !app->control_center_edge_tracking ||
       !edge_swipe_event_indev(app, event, &indev, &point) ||
       indev != app->control_center_indev) {
        return;
    }

    const int32_t distance = point.y - app->control_center_start_y;
    if(app->control_center_dragging && app->control_center) {
        const int32_t panel_distance = edge_swipe_panel_distance(app, distance);
        ui_control_center_set_drag(app->control_center, panel_distance);
        ui_control_center_finish_drag(app->control_center, panel_distance);
        set_content_frozen(app, false);
    }
    reset_edge_swipe(app);
}

static void app_edge_press_lost(lv_event_t *event)
{
    /* Some evdev drivers report PRESS_LOST when the finger leaves the first
     * object. Treat it as the end of the edge gesture so the panel still
     * settles instead of remaining half open. */
    app_edge_released(event);
}

static void update_status(ui_app_t *app)
{
    if(!app || !app->status_time || !app->status_wifi || !app->state) return;
    char time_text[8];
    ui_format_time(time_text, sizeof(time_text));
    lv_label_set_text(app->status_time, time_text);

    const app_model_t *model = app_state_model(app->state);
    if(model) ui_icon_set_visible(app->status_wifi, model->wifi);
}

static void clock_timer(lv_timer_t *timer)
{
    ui_app_t *app = timer ? lv_timer_get_user_data(timer) : NULL;
    if(app && app->content_frozen) return;
    if(app && app->bluetooth) {
        binbot_bluetooth_snapshot_t bluetooth;
        if(binbot_bluetooth_manager_snapshot(app->bluetooth, &bluetooth) &&
           bluetooth.ready && bluetooth.available && !bluetooth.busy &&
           bluetooth.powered != app_state_model(app->state)->bluetooth)
            app_state_toggle_control(app->state, APP_CONTROL_BLUETOOTH);
    }
    update_status(app);
}

size_t ui_music_track_count(void)
{
    return UI_MUSIC_TRACK_COUNT;
}

const ui_music_track_t *ui_music_track_at(size_t index)
{
    return index < UI_MUSIC_TRACK_COUNT ? &music_tracks[index] : NULL;
}

const ui_music_track_t *ui_music_current_track(const ui_app_t *app)
{
    if(!app || app->music.track_index >= UI_MUSIC_TRACK_COUNT) return &music_tracks[0];
    return &music_tracks[app->music.track_index];
}

static const char *music_current_path(const ui_app_t *app)
{
    if(!app) return NULL;
    if(app->music.track_index == 0U) {
        const char *override = getenv("BINBOT_MUSIC_FILE");
        if(override && override[0]) return override;
    }
    return ui_music_current_track(app)->path;
}

static bool music_send(ui_app_t *app, binbot_audio_music_command_t command,
                       uint32_t position_ms, const char *path)
{
    if(!app || !app->audio_client) return false;
    bool sent = binbot_audio_client_music_control(app->audio_client, command,
                                                  position_ms, path);
    if(sent && command == BINBOT_AUDIO_MUSIC_PLAY) {
        app->noise.playing = 0;
        if(app->route == UI_ROUTE_NOISE && app->current.context)
            ui_noise_screen_sync(app->current.context);
    }
    return sent;
}

bool ui_app_pronounce_word(ui_app_t *app, const char *word, app_vocabulary_accent_t accent)
{
    if(!app) return false;
    if(app->voice_state >= BINBOT_DIALOG_STATE_LISTENING &&
       app->voice_state <= BINBOT_DIALOG_STATE_CANCELLING) {
        ui_app_show_toast(app, "请先结束语音对话");
        return false;
    }
    char path[256];
    if(!app_vocabulary_audio_path(word, accent, path, sizeof(path))) {
        ui_app_show_toast(app, "这个单词的发音尚未收录");
        return false;
    }
    if(!binbot_audio_client_pronunciation(app->audio_client, true, path)) {
        ui_app_show_toast(app, "音频服务未连接");
        return false;
    }
    /* The one-shot uses the MP3 worker. Pause the music UI timer so it cannot
     * automatically advance or repeat the old song over a vocabulary clip. */
    app->music.playing = false;
    app->music.active = false;
    app->noise.playing = 0;
    music_notify(app);
    return true;
}

void ui_app_stop_pronunciation(ui_app_t *app)
{
    if(app) (void)binbot_audio_client_pronunciation(app->audio_client, false, NULL);
}

void ui_app_handle_noise_state(ui_app_t *app, const binbot_audio_noise_state_t *state)
{
    if(!app || !binbot_audio_noise_is_valid(state)) return;
    app->noise = *state;
    if(state->playing) {
        app->music.playing = false;
        app->music.active = false;
        music_notify(app);
    }
    if(app->route == UI_ROUTE_NOISE && app->current.context && !app->content_frozen)
        ui_noise_screen_sync(app->current.context);
}

bool ui_noise_apply(ui_app_t *app, const binbot_audio_noise_state_t *state)
{
    if(!app || !binbot_audio_noise_is_valid(state)) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_NOISE_CONTROL,
                             sizeof(packet.payload.noise), 0,
                             (uint64_t)platform_monotonic_ms() * 1000000ULL);
    packet.payload.noise = *state;
    if(!app->audio_client || !binbot_audio_client_send(app->audio_client, &packet)) {
        ui_app_show_toast(app, "音频服务未连接");
        return false;
    }
    ui_app_handle_noise_state(app, state);
    return true;
}

static void music_notify(ui_app_t *app)
{
    if(!app) return;
    if(!app->content_frozen && app->route == UI_ROUTE_MUSIC && app->current.context) {
        ui_music_screen_sync(app->current.context);
    }
    if(app->control_center && ui_control_center_is_active(app->control_center)) {
        ui_control_center_music_update(app->control_center);
    }
}

void ui_music_toggle_playback(ui_app_t *app)
{
    if(!app) return;
    if(app->music.playing) {
        if(!music_send(app, BINBOT_AUDIO_MUSIC_PAUSE, 0, NULL)) {
            ui_app_show_toast(app, "音频服务未连接");
            return;
        }
        app->music.playing = false;
        music_notify(app);
        return;
    }

    const char *path = music_current_path(app);
    if(!path) {
        ui_app_show_toast(app, "此曲目尚未配置音频");
        return;
    }
    if(!music_send(app, BINBOT_AUDIO_MUSIC_PLAY,
                   app->music.position_seconds * 1000U, path)) {
        ui_app_show_toast(app, "音频服务未连接");
        return;
    }
    app->music.active = true;
    app->music.playing = true;
    if(app->music_timer) lv_timer_reset(app->music_timer);
    music_notify(app);
}

void ui_music_select_track(ui_app_t *app, size_t index)
{
    if(!app || UI_MUSIC_TRACK_COUNT == 0) return;
    const bool was_playing = app->music.playing;
    if(app->music.active) {
        (void)music_send(app, BINBOT_AUDIO_MUSIC_STOP, 0, NULL);
    }

    app->music.track_index = index % UI_MUSIC_TRACK_COUNT;
    app->music.position_seconds = 0;
    app->music.active = false;
    app->music.playing = false;

    if(was_playing) {
        const char *path = music_current_path(app);
        if(!path) {
            ui_app_show_toast(app, "此曲目尚未配置音频");
        }
        else if(music_send(app, BINBOT_AUDIO_MUSIC_PLAY, 0, path)) {
            app->music.active = true;
            app->music.playing = true;
            if(app->music_timer) lv_timer_reset(app->music_timer);
        }
        else {
            ui_app_show_toast(app, "音频服务未连接");
        }
    }
    music_notify(app);
}

void ui_music_seek(ui_app_t *app, uint32_t position_seconds)
{
    if(!app) return;
    const ui_music_track_t *track = ui_music_current_track(app);
    if(position_seconds > track->duration_seconds) {
        position_seconds = track->duration_seconds;
    }
    if(app->music.active &&
       !music_send(app, BINBOT_AUDIO_MUSIC_SEEK, position_seconds * 1000U, NULL)) {
        ui_app_show_toast(app, "音频服务未连接");
        return;
    }
    app->music.position_seconds = position_seconds;
    if(app->music_timer) lv_timer_reset(app->music_timer);
    music_notify(app);
}

void ui_music_previous(ui_app_t *app)
{
    if(!app) return;
    if(app->music.position_seconds > 3U) {
        ui_music_seek(app, 0);
        return;
    }
    ui_music_select_track(app,
                          (app->music.track_index + UI_MUSIC_TRACK_COUNT - 1U) %
                              UI_MUSIC_TRACK_COUNT);
}

void ui_music_next(ui_app_t *app)
{
    if(!app) return;
    ui_music_select_track(app, (app->music.track_index + 1U) % UI_MUSIC_TRACK_COUNT);
}

bool ui_app_apply_remote_music(ui_app_t *app,
                               binbot_audio_remote_action_t action,
                               uint32_t value)
{
    if(!app) return false;
    switch(action) {
        case BINBOT_REMOTE_MUSIC_PLAY:
            if(!app->music.playing) ui_music_toggle_playback(app);
            return app->music.playing;
        case BINBOT_REMOTE_MUSIC_PAUSE:
            if(app->music.playing) ui_music_toggle_playback(app);
            return !app->music.playing;
        case BINBOT_REMOTE_MUSIC_STOP:
            if(app->music.active &&
               !music_send(app, BINBOT_AUDIO_MUSIC_STOP, 0, NULL)) {
                return false;
            }
            app->music.position_seconds = 0;
            app->music.active = false;
            app->music.playing = false;
            music_notify(app);
            return true;
        case BINBOT_REMOTE_MUSIC_PREVIOUS:
            ui_music_previous(app);
            return true;
        case BINBOT_REMOTE_MUSIC_NEXT:
            ui_music_next(app);
            return true;
        case BINBOT_REMOTE_MUSIC_SEEK:
            ui_music_seek(app, value);
            return true;
        case BINBOT_REMOTE_MUSIC_SELECT:
            if(value >= UI_MUSIC_TRACK_COUNT) return false;
            ui_music_select_track(app, value);
            return app->music.track_index == value;
        default:
            return false;
    }
}

void ui_app_get_music_snapshot(const ui_app_t *app,
                               ui_app_music_snapshot_t *snapshot)
{
    if(!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if(!app) return;
    snapshot->track_index = (uint8_t)app->music.track_index;
    snapshot->position_seconds = app->music.position_seconds;
    snapshot->active = app->music.active;
    snapshot->playing = app->music.playing;
}

void ui_app_reload_home_gif(ui_app_t *app)
{
    if(app && app->route == UI_ROUTE_HOME) ui_app_rebuild(app);
}

void ui_music_toggle_repeat(ui_app_t *app)
{
    if(!app) return;
    app->music.repeating = !app->music.repeating;
    music_notify(app);
}

void ui_music_toggle_like(ui_app_t *app)
{
    if(!app) return;
    app->music.liked = !app->music.liked;
    music_notify(app);
}

static void music_timer(lv_timer_t *timer)
{
    ui_app_t *app = timer ? lv_timer_get_user_data(timer) : NULL;
    if(!app || !app->music.playing) return;

    const ui_music_track_t *track = ui_music_current_track(app);
    if(app->music.position_seconds < track->duration_seconds) {
        app->music.position_seconds++;
    }
    if(app->music.position_seconds < track->duration_seconds) {
        music_notify(app);
        return;
    }

    if(app->music.repeating) {
        const char *path = music_current_path(app);
        app->music.position_seconds = 0;
        if(!path || !music_send(app, BINBOT_AUDIO_MUSIC_PLAY, 0, path)) {
            app->music.active = false;
            app->music.playing = false;
            ui_app_show_toast(app, "音频服务未连接");
        }
        music_notify(app);
        return;
    }
    ui_music_next(app);
}

static void state_changed(uint32_t changes, const app_model_t *model, void *user_data)
{
    ui_app_t *app = user_data;
    if(!app || !model) return;
    if(changes & APP_CHANGE_GESTURES) ui_gesture_config_changed(app->gestures);
    if(app->content_frozen) {
        app->frozen_changes |= changes;
        return;
    }
    if(changes & (APP_CHANGE_CLOCK | APP_CHANGE_CONTROLS)) update_status(app);
    if(app->current.update) app->current.update(app->current.context, model, changes);
    if(app->control_center && ui_control_center_is_active(app->control_center)) {
        ui_control_center_update(app->control_center, model, changes);
    }
}

static void set_content_frozen(ui_app_t *app, bool frozen)
{
    if(!app || app->content_frozen == frozen) return;

    app->content_frozen = frozen;
    if(frozen) {
        app->frozen_changes = APP_CHANGE_NONE;
        if(app->clock_timer) lv_timer_pause(app->clock_timer);
        if(app->current.set_paused) app->current.set_paused(app->current.context, true);
        return;
    }

    if(app->clock_timer) lv_timer_resume(app->clock_timer);
    if(app->current.set_paused) app->current.set_paused(app->current.context, false);

    /* Apply everything accumulated while the finger was dragging.  Keeping
     * the original change mask avoids manufacturing a pomodoro phase event
     * merely because the panel was opened. */
    const uint32_t changes = app->frozen_changes;
    app->frozen_changes = APP_CHANGE_NONE;
    update_status(app);
    music_notify(app);
    if(changes != APP_CHANGE_NONE) {
        const app_model_t *model = app_state_model(app->state);
        if(model && app->current.update) {
            app->current.update(app->current.context, model, changes);
        }
        if(model && app->control_center &&
           ui_control_center_is_active(app->control_center)) {
            ui_control_center_update(app->control_center, model, changes);
        }
    }
}

void ui_app_navigate(ui_app_t *app, ui_route_t route)
{
    if(!app) return;
    ui_screensaver_wake(app->screensaver);
    if(app->content_frozen) set_content_frozen(app, false);
    if(app->control_center_edge_tracking) reset_edge_swipe(app);
    app->pending_route = route;
    app->pending_route_is_back = false;
    app->pending_back_history_len = 0;
    if(app->route_pending) return;

    app->route_pending = true;
    if(lv_async_call(apply_route_async, app) != LV_RESULT_OK) {
        app->route_pending = false;
        fprintf(stderr, "ui: failed to queue route %d\n", (int)route);
    }
}

static void apply_pending_route(ui_app_t *app)
{
    if(!app->route_pending) return;
    lv_async_call_cancel(apply_route_async, app);
    apply_route_async(app);
}

bool ui_app_show_watchface(ui_app_t *app, const char *theme_id)
{
    if(!app) return false;
    binbot_ambient_display_t *display = ui_screensaver_display(app->screensaver);
    if(!binbot_ambient_theme_find(display, theme_id)) return false;

    apply_pending_route(app);
    ui_gesture_dismiss_popup(app->gestures);
    char previous[32] = {0};
    if(ui_screensaver_active(app->screensaver))
        snprintf(previous, sizeof(previous), "%s", binbot_ambient_current_theme(display));
    if(ui_screensaver_preview(app->screensaver, theme_id)) return true;

    if(previous[0]) (void)ui_screensaver_preview(app->screensaver, previous);
    else ui_screensaver_wake(app->screensaver);
    return false;
}

bool ui_app_show_pomodoro(ui_app_t *app)
{
    if(!app) return false;
    ui_gesture_dismiss_popup(app->gestures);
    if(app->route == UI_ROUTE_POMODORO && app->current.root && !app->route_pending) {
        ui_screensaver_wake(app->screensaver);
        ui_app_close_control_center(app);
        return true;
    }
    ui_app_navigate(app, UI_ROUTE_POMODORO);
    apply_pending_route(app);
    return app->route == UI_ROUTE_POMODORO && app->current.root;
}

void ui_app_go_back(ui_app_t *app)
{
    if(!app) return;
    ui_screensaver_wake(app->screensaver);
    if(app->content_frozen) set_content_frozen(app, false);
    if(app->control_center_edge_tracking) reset_edge_swipe(app);
    if(app->route_pending) return;

    ui_route_t target = UI_ROUTE_HOME;
    size_t back_history_len = app->route_history_len;
    bool has_fixed_parent = false;
    switch(app->route) {
        case UI_ROUTE_GESTURES:
        case UI_ROUTE_SCREENSAVER_SETTINGS:
        case UI_ROUTE_ABOUT:
            target = UI_ROUTE_SETTINGS;
            has_fixed_parent = true;
            break;
        case UI_ROUTE_DIAGNOSTICS:
            target = UI_ROUTE_ABOUT;
            has_fixed_parent = true;
            break;
        case UI_ROUTE_CALENDAR_DETAIL:
            target = app->calendar_from_home ? UI_ROUTE_HOME : UI_ROUTE_CALENDAR;
            has_fixed_parent = true;
            break;
        default:
            if(app->route_history_len > 0)
                target = app->route_history[app->route_history_len - 1];
            break;
    }
    if(has_fixed_parent) {
        size_t index = app->route_history_len;
        while(index > 0 && app->route_history[index - 1] != target) --index;
        back_history_len = index > 0 ? index : 0;
    }
    if(target == app->route) return;

    app->pending_route = target;
    app->pending_route_is_back = true;
    app->pending_back_history_len = back_history_len;
    app->route_pending = true;
    if(lv_async_call(apply_route_async, app) != LV_RESULT_OK) {
        app->route_pending = false;
        app->pending_route_is_back = false;
        app->pending_back_history_len = 0;
        fprintf(stderr, "ui: failed to queue back route %d\n", (int)target);
    }
}

void ui_app_rebuild(ui_app_t *app)
{
    /* A rebuild requested while another route is queued would overwrite that
     * destination.  The queued route already receives APP_CHANGE_ALL. */
    if(app && !app->route_pending) ui_app_navigate(app, app->route);
}

void ui_app_open_control_center(ui_app_t *app)
{
    if(!app || app->route_pending || ui_screensaver_active(app->screensaver) ||
       ui_gesture_popup_active(app->gestures)) return;
    if(app->control_center) {
        if(!ui_control_center_is_active(app->control_center)) {
            ui_control_center_reopen(app->control_center);
        }
        ui_calendar_notice_raise(app->calendar_notice);
        ui_module_notice_raise(app->module_notice);
        return;
    }
    app->control_center = ui_control_center_open(app);
    ui_calendar_notice_raise(app->calendar_notice);
    ui_module_notice_raise(app->module_notice);
}

bool ui_app_open_control_center_drag(ui_app_t *app, int32_t distance)
{
    if(!app || app->route_pending || ui_screensaver_active(app->screensaver) ||
       ui_gesture_popup_active(app->gestures)) return false;
    if(app->control_center) {
        if(ui_control_center_is_active(app->control_center)) return false;
        ui_control_center_reopen_drag(app->control_center, distance);
        ui_calendar_notice_raise(app->calendar_notice);
        ui_module_notice_raise(app->module_notice);
        return ui_control_center_is_active(app->control_center);
    }
    app->control_center = ui_control_center_open_drag(app, distance);
    ui_calendar_notice_raise(app->calendar_notice);
    ui_module_notice_raise(app->module_notice);
    return app->control_center != NULL;
}

void ui_app_update_control_center_drag(ui_app_t *app, int32_t distance)
{
    if(app && app->control_center && ui_control_center_is_active(app->control_center)) {
        ui_control_center_set_drag(app->control_center, distance);
    }
}

void ui_app_finish_control_center_drag(ui_app_t *app, int32_t distance)
{
    if(app && app->control_center && ui_control_center_is_active(app->control_center)) {
        ui_control_center_finish_drag(app->control_center, distance);
    }
}

void ui_app_set_control_center_drag_active(ui_app_t *app, bool active)
{
    set_content_frozen(app, active);
}

void ui_app_close_control_center(ui_app_t *app)
{
    if(!app || !app->control_center ||
       !ui_control_center_is_active(app->control_center)) return;
    if(app->content_frozen) set_content_frozen(app, false);
    ui_control_center_deactivate(app->control_center);
    if(app->control_center_edge_tracking) reset_edge_swipe(app);
    lv_async_call_cancel(release_control_center, app);
    (void)lv_async_call(release_control_center, app);
}

static void toast_timer(lv_timer_t *timer)
{
    lv_obj_t *toast = timer ? lv_timer_get_user_data(timer) : NULL;
    if(toast && lv_obj_is_valid(toast)) lv_obj_delete(toast);
    if(timer) lv_timer_delete(timer);
}

void ui_app_show_toast(ui_app_t *app, const char *message)
{
    if(!app || !app->root || !message) return;
    lv_obj_t *toast = ui_box(app->root, 180, 630, 360, 54, 0x20242d, 27);
    if(!toast) return;
    lv_obj_move_foreground(toast);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    ui_centered_label(toast, message, ui_font_body(), 0xffffff);
    ui_screensaver_raise(app->screensaver);
    lv_timer_t *timer = lv_timer_create(toast_timer, 1700, toast);
    if(!timer) lv_obj_delete(toast);
}

void ui_app_set_audio_client(ui_app_t *app, binbot_audio_client_t *client)
{
    if(app) app->audio_client = client;
}

bool ui_app_request_wake(ui_app_t *app)
{
    if(app && app->route == UI_ROUTE_DIAGNOSTICS) return false;
    if(!app || !app->audio_client ||
       binbot_audio_client_fd(app->audio_client) < 0) {
        if(app) ui_app_show_toast(app, "音频服务未连接");
        return false;
    }
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_WAKE_REQUEST,
                             sizeof(packet.payload.wake), 0,
                             (uint64_t)platform_monotonic_ms() * 1000000ULL);
    packet.payload.wake.hotword_index = 0;
    if(!binbot_audio_client_send(app->audio_client, &packet)) {
        ui_app_show_toast(app, "唤醒请求发送失败");
        return false;
    }
    return true;
}

bool ui_app_finish_voice_input(ui_app_t *app)
{
    if(!app || app->voice_state < BINBOT_DIALOG_STATE_LISTENING ||
       app->voice_state > BINBOT_DIALOG_STATE_SPEAKING) return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_INPUT_END, 0, 0,
                             (uint64_t)platform_monotonic_ms() * 1000000ULL);
    if(!binbot_audio_client_send(app->audio_client, &packet)) {
        ui_app_show_toast(app, "结束收音请求发送失败");
        return false;
    }
    return true;
}

void ui_app_handle_music_state(ui_app_t *app, int state,
                               uint32_t position_ms, uint32_t duration_ms)
{
    (void)duration_ms;
    if(!app) return;
    const ui_music_track_t *track = ui_music_current_track(app);
    app->music.position_seconds = position_ms / 1000U;
    if(app->music.position_seconds > track->duration_seconds) {
        app->music.position_seconds = track->duration_seconds;
    }
    app->music.active = state != 0;
    app->music.playing = state == 1;
    music_notify(app);
}

void ui_app_handle_wake_detected(ui_app_t *app, int hotword_index)
{
    (void)hotword_index;
    if(!app || app->route == UI_ROUTE_DIAGNOSTICS) return;
    ui_screensaver_wake(app->screensaver);
    app->voice_listening = true;
    ui_app_close_control_center(app);
    if(app->route != UI_ROUTE_ASSISTANT || app->route_pending)
        ui_app_navigate(app, UI_ROUTE_ASSISTANT);
    ui_app_show_toast(app, "已唤醒，正在聆听…");
}

void ui_app_handle_dialog_state(ui_app_t *app, int state)
{
    if(!app || state < BINBOT_DIALOG_STATE_DISCONNECTED || state > BINBOT_DIALOG_STATE_ERROR) return;
    if(state != BINBOT_DIALOG_STATE_ERROR && app_calendar_set_connected(&app->calendar,
                                                                       state != BINBOT_DIALOG_STATE_DISCONNECTED))
        state_changed(APP_CHANGE_CALENDAR, app_state_model(app->state), app);
    if(app->voice_state == state) return;
    if(state != BINBOT_DIALOG_STATE_SPEAKING) ui_chat_reset_playback(app);
    if(app->voice_state >= BINBOT_DIALOG_STATE_LISTENING &&
       app->voice_state <= BINBOT_DIALOG_STATE_SPEAKING) lv_display_trigger_activity(NULL);
    app->voice_state = state;
    app->voice_listening = state == 2;
    if(app->route == UI_ROUTE_DIAGNOSTICS) return;
    if(state >= 2 && state <= 4) {
        ui_screensaver_wake(app->screensaver);
        ui_app_close_control_center(app);
        if(app->route != UI_ROUTE_ASSISTANT || app->route_pending)
            ui_app_navigate(app, UI_ROUTE_ASSISTANT);
    }
    else if(app->route == UI_ROUTE_ASSISTANT && !app->route_pending) {
        if(!app->content_frozen) ui_assistant_screen_sync(app->current.context);
    }
    if(state == 6) ui_app_show_toast(app, "语音服务暂时不可用");
}

bool ui_app_handle_calendar_state(ui_app_t *app, const binbot_audio_calendar_state_t *state)
{
    if(!app || !app_calendar_receive(&app->calendar, state, time(NULL))) return false;
    state_changed(APP_CHANGE_CALENDAR, app_state_model(app->state), app);
    ui_calendar_notice_show(app->calendar_notice, &app->calendar.changes);
    return true;
}

void ui_app_handle_encoder_count(ui_app_t *app, int32_t count, bool input_event)
{
    if(!app) return;
    if(!app->encoder_count_valid) {
        app->last_encoder_count = count;
        app->encoder_count_valid = true;
        return;
    }
    if(!input_event) return;

    int64_t steps = (int64_t)count - app->last_encoder_count;
    app->last_encoder_count = count;
    if(steps != 0 && steps >= -32 && steps <= 32) {
        bool sleeping = ui_screensaver_active(app->screensaver);
        ui_screensaver_wake(app->screensaver);
        if(sleeping) return;
    }
    if(steps == 0 || steps < -32 || steps > 32 ||
       app->route != UI_ROUTE_APPS || app->route_pending ||
       (app->control_center && ui_control_center_is_active(app->control_center))) {
        return;
    }
    ui_apps_screen_rotate(app, (int32_t)steps);
}

void ui_app_handle_slave_input(ui_app_t *app, uint8_t adc_key, bool encoder_down)
{
    if(!app) return;
    const bool active = adc_key != 0U || encoder_down;
    if(!active) {
        /* A neutral report rearms the next physical press. */
        app->last_slave_adc_key = 0;
        app->last_slave_encoder_down = false;
        app->slave_input_latched = false;
        return;
    }

    /* Event reports are state based.  Ignore duplicate reports for the same
     * decoded input while it remains held (or while an ADC key is settling). */
    if(app->slave_input_latched &&
       app->last_slave_adc_key == adc_key &&
       app->last_slave_encoder_down == encoder_down) {
        return;
    }
    app->last_slave_adc_key = adc_key;
    app->last_slave_encoder_down = encoder_down;
    app->slave_input_latched = true;
    ui_screensaver_wake(app->screensaver);

    /* SW1 and the encoder push button are dedicated voice-wake affordances.
     * The remaining keys retain their existing diagnostic behavior until they
     * are assigned to another product action. */
    if(adc_key == 1U || encoder_down) {
        if(ui_app_request_wake(app)) {
            ui_app_show_toast(app, "已唤醒，正在聆听…");
        }
    }
    else if(adc_key >= 1U && adc_key <= 3U) {
        char message[32];
        snprintf(message, sizeof(message), "按键 SW%u 已按下", (unsigned)adc_key);
        ui_app_show_toast(app, message);
    }
}

ui_app_t *ui_app_create(app_state_t *state)
{
    if(!state) return NULL;
    ui_app_t *app = calloc(1, sizeof(*app));
    if(!app) return NULL;

    ui_theme_initialize();
    app->state = state;
    app->modules = app_modules_create(NULL);
    app->bluetooth = binbot_bluetooth_manager_create();
    app->voice_state = 0;
    app->noise.master = 70;
    app->noise.levels[0] = 55;
    app->noise.levels[4] = 35;
    app->root = lv_screen_active();
    lv_obj_remove_style_all(app->root);
    lv_obj_set_size(app->root, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(app->root, lv_color_hex(UI_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(app->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(app->root, LV_OBJ_FLAG_SCROLLABLE);
    /* Header backgrounds hit the screen itself. Keep that press while an
     * on-demand overlay appears, just as clickable page children already do. */
    lv_obj_add_flag(app->root, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(app->root, app_edge_pressed, LV_EVENT_PRESSED, app);
    lv_obj_add_event_cb(app->root, app_edge_pressing, LV_EVENT_PRESSING, app);
    lv_obj_add_event_cb(app->root, app_edge_released, LV_EVENT_RELEASED, app);
    lv_obj_add_event_cb(app->root, app_edge_press_lost, LV_EVENT_PRESS_LOST, app);

    lv_obj_t *status = ui_box(app->root, 0, 0, UI_WIDTH, UI_STATUS_HEIGHT,
                              UI_COLOR_BACKGROUND, 0);
    app->status_bar = status;
    app->status_time = ui_label(status, "--:--", ui_font_status(), 0x3a3d45, 28, 11);
    lv_obj_t *open = ui_button(status, 320, 3, 80, 34, UI_COLOR_BACKGROUND, 17);
    lv_obj_set_style_bg_opa(open, LV_OPA_TRANSP, 0);
    lv_obj_t *down = ui_icon(open, UI_ICON_CHEVRON_DOWN, 16, 0x9a9faa);
    lv_obj_center(down);
    lv_obj_add_event_cb(open, control_center_clicked, LV_EVENT_CLICKED, app);
    app->status_signal = ui_icon(status, UI_ICON_SIGNAL, 16, 0x3a3d45);
    lv_obj_set_pos(app->status_signal, 618, 12);
    app->status_wifi = ui_icon(status, UI_ICON_WIFI, 16, 0x3a3d45);
    lv_obj_set_pos(app->status_wifi, 642, 12);
    app->status_battery = ui_icon(status, UI_ICON_BATTERY_FULL, 18, 0x3a3d45);
    lv_obj_set_pos(app->status_battery, 666, 11);

    app->page_layer = ui_box(app->root, 0, UI_STATUS_HEIGHT, UI_WIDTH, UI_PAGE_HEIGHT,
                             UI_COLOR_BACKGROUND, 0);
    app->screensaver = ui_screensaver_create(app);
    ui_route_t initial_route = initial_route_from_environment();
    app->route = initial_route;
    app->pending_route = initial_route;
    if(!activate_route(app, initial_route) &&
       (initial_route == UI_ROUTE_HOME || !activate_route(app, UI_ROUTE_HOME))) {
        ui_screensaver_destroy(app->screensaver);
        if(app->root && lv_obj_is_valid(app->root)) lv_obj_clean(app->root);
        ui_theme_shutdown();
        app_modules_destroy(app->modules);
        binbot_bluetooth_manager_destroy(app->bluetooth);
        free(app);
        return NULL;
    }
    app_state_subscribe(state, state_changed, app);
    app->clock_timer = lv_timer_create(clock_timer, 1000, app);
    app->music_timer = lv_timer_create(music_timer, 1000, app);
    app->module_notice = ui_module_notice_create(app);
    app->calendar_notice = ui_calendar_notice_create(app);
    app->gestures = ui_gesture_control_create(app);
    const char *initial = getenv("BINBOT_INITIAL_ROUTE");
    if(initial && strcmp(initial, "screensaver") == 0) ui_screensaver_show(app->screensaver);
    update_status(app);
    fprintf(stdout, "ui: BinUI presentation initialized\n");
    return app;
}

void ui_app_destroy(ui_app_t *app)
{
    if(!app) return;
    /* ui_app_navigate schedules a callback with this pointer as user data. */
    lv_async_call_cancel(apply_route_async, app);
    lv_async_call_cancel(release_control_center, app);
    app->route_pending = false;
    if(app->content_frozen) set_content_frozen(app, false);
    reset_edge_swipe(app);
    if(app->clock_timer) lv_timer_delete(app->clock_timer);
    if(app->music_timer) lv_timer_delete(app->music_timer);
    ui_calendar_notice_destroy(app->calendar_notice);
    ui_gesture_control_destroy(app->gestures);
    app->gestures = NULL;
    ui_screensaver_destroy(app->screensaver);
    if(app->control_center) {
        ui_control_center_destroy(app->control_center);
        app->control_center = NULL;
    }
    destroy_screen(&app->current);
    ui_chat_session_destroy(app->chat);
    ui_module_notice_destroy(app->module_notice);
    app_modules_destroy(app->modules);
    binbot_bluetooth_manager_destroy(app->bluetooth);
    if(app->root && lv_obj_is_valid(app->root)) lv_obj_clean(app->root);
    ui_theme_shutdown();
    free(app);
}

bool ui_app_studying_vocabulary(const ui_app_t *app)
{
    if(!app) return false;
    const app_vocabulary_t *deck = &app_state_model(app->state)->vocabulary;
    return deck->card_count && (app->route == UI_ROUTE_VOCABULARY ||
                                (!deck->complete && deck->attempted_mask));
}

app_modules_t *ui_app_modules(ui_app_t *app) { return app ? app->modules : NULL; }
app_firmware_t *ui_app_firmware(ui_app_t *app) { return app ? &app->firmware : NULL; }

void ui_app_set_slave_client(ui_app_t *app, binbot_slave_client_t *client)
{
    if(!app) return;
    app->slave_client = client;
    app->firmware.client = client;
}

bool ui_app_vibrate(ui_app_t *app, uint16_t duration_ms)
{
    if(!app || !app->slave_client || !duration_ms || duration_ms > 5000) return false;
    uint8_t payload[] = {(uint8_t)duration_ms, (uint8_t)(duration_ms >> 8)};
    return binbot_slave_client_send_request(app->slave_client, BINBOT_SLAVE_CMD_MOTOR_PULSE,
                                            payload, sizeof(payload)) != 0;
}
