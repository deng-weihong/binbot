#include "../ui_internal.h"
#include "binbot/wifi_manager.h"
#include "draw/snapshot/lv_snapshot.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CONTROL_TOGGLE_COUNT = 3,
    CONTROL_CENTER_PANEL_TOP = 22,
    CONTROL_CENTER_PANEL_HEIGHT = 587,
    CONTROL_CENTER_PANEL_HIDDEN_Y = -718,
    CONTROL_CENTER_PANEL_OFFSCREEN_Y = CONTROL_CENTER_PANEL_HIDDEN_Y - 54,
    CONTROL_CENTER_PANEL_TRAVEL = UI_CONTROL_CENTER_DRAG_MAX,
    CONTROL_CENTER_OPEN_THRESHOLD = 70,
    CONTROL_CENTER_DRAG_TRIGGER = 8,
    CONTROL_CENTER_CLOSE_THRESHOLD = 60,
    CONTROL_CENTER_FLICK_VELOCITY = 4,
    /* Leave a small touch zone at the bottom of the vertical slider for an
     * unambiguous minimum value when the touch panel reports the last point
     * just above the slider edge. */
    BRIGHTNESS_MIN_SNAP_PIXELS = 10,
};

typedef struct {
    struct ui_control_center *center;
    app_control_t control;
} control_binding_t;

struct ui_control_center {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *backdrop;
    lv_obj_t *panel;
    lv_obj_t *motion_background;
    lv_obj_t *motion_panel;
    lv_draw_buf_t *background_snapshot;
    lv_draw_buf_t *panel_snapshot;
    int32_t snapshot_margin;
    bool motion_cached;
    bool motion_settled;
    bool background_ready;
    app_model_t snapshot_model;
    ui_music_state_t snapshot_music;
    char snapshot_wifi_status[BINBOT_WIFI_SSID_MAX];
    app_model_t background_model;
    ui_music_state_t background_music;
    char background_time[6];
    lv_obj_t *toggles[CONTROL_TOGGLE_COUNT];
    lv_obj_t *toggle_icons[CONTROL_TOGGLE_COUNT];
    lv_obj_t *toggle_labels[CONTROL_TOGGLE_COUNT];
    lv_obj_t *toggle_status[CONTROL_TOGGLE_COUNT];
    lv_obj_t *toggle_circles[CONTROL_TOGGLE_COUNT];
    lv_obj_t *focus_button;
    lv_obj_t *focus_icon;
    lv_obj_t *focus_label;
    lv_obj_t *focus_status;
    lv_obj_t *focus_circle;
    lv_obj_t *brightness;
    lv_obj_t *volume;
    lv_obj_t *brightness_fill;
    lv_obj_t *volume_fill;
    lv_obj_t *brightness_value;
    lv_obj_t *volume_value;
    lv_obj_t *music_title;
    lv_obj_t *music_artist;
    lv_obj_t *music_status;
    lv_obj_t *music_play_icon;
    lv_obj_t *music_cover_index;
    lv_obj_t *close_button;
    binbot_wifi_manager_t *wifi;
    lv_timer_t *wifi_timer;
    lv_timer_t *close_delete_timer;
    bool active;
    bool close_after_animation;
    lv_indev_t *close_drag_indev;
    bool close_drag_tracking;
    bool close_dragging;
    bool close_drag_flick;
    bool suppress_click;
    int32_t close_drag_start_x;
    int32_t close_drag_start_y;
    int32_t close_drag_origin_y;
    int32_t close_drag_distance;
    /* A live edge drag keeps the dim layer at its initial opacity.  Changing
     * a full-screen object's opacity on every touch sample invalidates the
     * entire underlying page and makes the panel feel sticky. */
    bool drag_backdrop_locked;
    control_binding_t bindings[CONTROL_TOGGLE_COUNT];
};

static void close_delete_timer_cb(lv_timer_t *timer);

static bool take_snapshot(lv_obj_t *object, lv_color_format_t format,
                          lv_draw_buf_t **buffer)
{
    if(*buffer) {
        lv_image_cache_drop(*buffer);
        return lv_snapshot_take_to_draw_buf(object, format, *buffer) == LV_RESULT_OK;
    }
    *buffer = lv_snapshot_take(object, format);
    return *buffer != NULL;
}

static lv_obj_t *snapshot_image(lv_obj_t *parent, lv_draw_buf_t *buffer)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_image_set_src(image, buffer);
    return image;
}

static bool panel_snapshot_is_current(ui_control_center_t *center)
{
    const app_model_t *model = app_state_model(center->app->state);
    const app_model_t *previous = &center->snapshot_model;
    const ui_music_state_t *music = &center->app->music;
    const char *wifi_status = lv_label_get_text(center->toggle_status[0]);
    return center->panel_snapshot &&
       model->wifi == previous->wifi && model->bluetooth == previous->bluetooth &&
       model->dnd == previous->dnd && model->brightness == previous->brightness &&
       model->volume == previous->volume &&
       model->pomodoro.running == previous->pomodoro.running &&
       model->pomodoro.mode == previous->pomodoro.mode &&
       model->pomodoro.seconds_left == previous->pomodoro.seconds_left &&
       music->track_index == center->snapshot_music.track_index &&
       music->playing == center->snapshot_music.playing &&
       strcmp(wifi_status, center->snapshot_wifi_status) == 0;
}

static bool cache_panel(ui_control_center_t *center)
{
    if(panel_snapshot_is_current(center)) return true;
    if(!take_snapshot(center->panel, LV_COLOR_FORMAT_ARGB8888,
                      &center->panel_snapshot)) return false;
    center->snapshot_model = *app_state_model(center->app->state);
    center->snapshot_music = center->app->music;
    snprintf(center->snapshot_wifi_status, sizeof(center->snapshot_wifi_status), "%s",
              lv_label_get_text(center->toggle_status[0]));
    center->snapshot_margin = ((int32_t)center->panel_snapshot->header.w -
                                lv_obj_get_width(center->panel)) / 2;
    return true;
}

static bool background_is_current(ui_control_center_t *center)
{
    char time[6];
    ui_format_time(time, sizeof(time));
    const ui_music_state_t *music = &center->app->music;
    return center->background_ready &&
           memcmp(&center->background_model, app_state_model(center->app->state),
                   sizeof(center->background_model)) == 0 &&
           music->track_index == center->background_music.track_index &&
           music->position_seconds == center->background_music.position_seconds &&
           music->playing == center->background_music.playing &&
           music->liked == center->background_music.liked &&
           music->repeating == center->background_music.repeating &&
           strcmp(time, center->background_time) == 0;
}

static void begin_cached_motion(ui_control_center_t *center)
{
    center->motion_settled = false;
    if(center->motion_cached) return;
    if(!cache_panel(center)) return;

    /* Keep the live tree available for pointer events while its cached pixels
     * move. An opaque background snapshot also avoids repainting the page and
     * scaling its blurred backdrop on every input sample. */
    lv_obj_set_style_opa_layered(center->panel, LV_OPA_TRANSP, 0);
    if(!center->background_ready) {
        if(!take_snapshot(center->app->root, LV_COLOR_FORMAT_XRGB8888,
                          &center->background_snapshot)) {
            lv_obj_set_style_opa_layered(center->panel, LV_OPA_COVER, 0);
            return;
        }
        center->background_model = *app_state_model(center->app->state);
        center->background_music = center->app->music;
        ui_format_time(center->background_time, sizeof(center->background_time));
        center->background_ready = true;
    }
    if(!center->motion_background)
        center->motion_background = snapshot_image(center->root, center->background_snapshot);
    else
        lv_image_set_src(center->motion_background, center->background_snapshot);
    /* LVGL's image cover check inherits the background opacity check. Mark
     * this opaque bitmap as covering so the live page is skipped entirely. */
    lv_obj_set_style_bg_opa(center->motion_background, LV_OPA_COVER, 0);
    lv_obj_set_pos(center->motion_background, 0, 0);
    if(lv_obj_get_index(center->motion_background) > lv_obj_get_index(center->panel))
        lv_obj_move_to_index(center->motion_background, lv_obj_get_index(center->panel));
    lv_obj_remove_flag(center->motion_background, LV_OBJ_FLAG_HIDDEN);
    if(!center->motion_panel)
        center->motion_panel = snapshot_image(center->root, center->panel_snapshot);
    else
        lv_image_set_src(center->motion_panel, center->panel_snapshot);
    lv_obj_set_pos(center->motion_panel,
                    lv_obj_get_x(center->panel) - center->snapshot_margin,
                    lv_obj_get_y(center->panel) - center->snapshot_margin);
    lv_obj_remove_flag(center->motion_panel, LV_OBJ_FLAG_HIDDEN);
    center->motion_cached = true;
    center->drag_backdrop_locked = true;
}

static void end_cached_motion(ui_control_center_t *center, bool keep_background)
{
    if(!center->motion_cached) return;
    lv_obj_add_flag(center->motion_panel, LV_OBJ_FLAG_HIDDEN);
    if(!keep_background) lv_obj_add_flag(center->motion_background, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa_layered(center->panel, LV_OPA_COVER, 0);
    center->motion_cached = false;
    center->motion_settled = false;
}

static void restore_settled_panel(ui_control_center_t *center)
{
    if(!center->motion_settled) return;
    end_cached_motion(center, true);
    center->drag_backdrop_locked = false;
}

static void panel_y_exec(void *var, int32_t value)
{
    ui_control_center_t *center = var;
    if(center && center->panel && lv_obj_is_valid(center->panel)) {
        lv_obj_set_y(center->panel, value);
        if(center->motion_cached)
            lv_obj_set_y(center->motion_panel, value - center->snapshot_margin);
        int32_t shown = value - CONTROL_CENTER_PANEL_HIDDEN_Y;
        if(shown < 0) shown = 0;
        if(shown > CONTROL_CENTER_PANEL_TRAVEL) shown = CONTROL_CENTER_PANEL_TRAVEL;
        lv_opa_t opacity = (lv_opa_t)((shown * LV_OPA_50) /
                                      CONTROL_CENTER_PANEL_TRAVEL);
        if(!center->drag_backdrop_locked &&
           lv_obj_get_style_bg_opa(center->root, 0) != opacity) {
            lv_obj_set_style_bg_opa(center->root, opacity, 0);
        }
        if(!center->drag_backdrop_locked && center->backdrop)
            lv_obj_set_style_image_opa(center->backdrop, (shown * LV_OPA_COVER) / CONTROL_CENTER_PANEL_TRAVEL, 0);
    }
}

static void panel_animation_completed(lv_anim_t *animation)
{
    ui_control_center_t *center = animation ? lv_anim_get_user_data(animation) : NULL;
    if(!center || !center->active) return;
    /* The final animation value has changed the style, but its coordinates
     * are not committed until layout runs. Do not restore the previous frame. */
    lv_obj_update_layout(center->panel);
    /* Keep identical cached pixels at the endpoint. Live controls take over
     * on interaction or a visible state change, without a release-frame redraw. */
    if(!center->close_after_animation && center->motion_cached &&
       panel_snapshot_is_current(center)) {
        center->motion_settled = true;
        return;
    }
    /* The opaque composition remains behind the live controls while open;
     * switching back must not repaint and rescale the entire blurred page. */
    end_cached_motion(center, !center->close_after_animation);
    center->drag_backdrop_locked = false;
    panel_y_exec(center, lv_obj_get_y(center->panel));
    if(!center->close_after_animation) return;
    center->close_after_animation = false;
    /* Let LVGL flush one fully transparent, completely off-screen frame
     * before parking the overlay.  The tree stays alive while this route is
     * active, avoiding the full-screen invalidation caused by deleting it. */
    center->close_delete_timer = lv_timer_create(close_delete_timer_cb, 20, center);
    if(!center->close_delete_timer && center->app &&
       center->app->control_center == center) {
        ui_app_close_control_center(center->app);
    }
}

static void close_delete_timer_cb(lv_timer_t *timer)
{
    ui_control_center_t *center = timer ? lv_timer_get_user_data(timer) : NULL;
    if(!center) {
        if(timer) lv_timer_delete(timer);
        return;
    }
    center->close_delete_timer = NULL;
    lv_timer_delete(timer);
    if(center->active && center->app && center->app->control_center == center) {
        ui_app_close_control_center(center->app);
    }
}

static void animate_panel_to(ui_control_center_t *center, int32_t start, int32_t end,
                             uint32_t duration, bool close_after)
{
    if(!center || !center->panel || !lv_obj_is_valid(center->panel)) return;
    lv_anim_delete(center, NULL);
    center->close_after_animation = close_after;
    begin_cached_motion(center);
    if(center->motion_cached) center->drag_backdrop_locked = true;
    /* Moving a closing panel must not animate the full-screen backdrop.  On
     * the single-buffer framebuffer this would invalidate and flush the whole
     * display on every animation tick, which can expose a transient frame at
     * the end of the gesture.  The backdrop is cleared when the panel is
     * already parked off-screen. */
    if(close_after) center->drag_backdrop_locked = true;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, center);
    lv_anim_set_exec_cb(&animation, panel_y_exec);
    lv_anim_set_values(&animation, start, end);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_user_data(&animation, center);
    lv_anim_set_completed_cb(&animation, panel_animation_completed);
    lv_anim_start(&animation);
}

static void animate_panel_in(ui_control_center_t *center)
{
    if(!center || !center->panel || !lv_obj_is_valid(center->panel)) return;
    lv_obj_set_y(center->panel, CONTROL_CENTER_PANEL_HIDDEN_Y);
    panel_y_exec(center, CONTROL_CENTER_PANEL_HIDDEN_Y);
    animate_panel_to(center, CONTROL_CENTER_PANEL_HIDDEN_Y,
                     CONTROL_CENTER_PANEL_TOP, 280, false);
}

static void close_async(void *user_data)
{
    ui_app_close_control_center(user_data);
}

static bool event_was_dragged(lv_event_t *event)
{
    lv_indev_t *indev = event ? lv_event_get_indev(event) : NULL;
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
           lv_indev_get_press_moved(indev);
}

static void close_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || !center->active || !center->app || center->suppress_click ||
       event_was_dragged(event)) return;
    if(lv_event_get_current_target(event) == center->root &&
       lv_event_get_target(event) != center->root) {
        return;
    }
    lv_async_call(close_async, center->app);
}

static void clear_gesture_pressed_state(void)
{
    lv_obj_t *object = lv_indev_get_active_obj();
    while(object) {
        lv_obj_remove_state(object, LV_STATE_PRESSED);
        object = lv_obj_get_parent(object);
    }
}

static void enable_event_bubble(lv_obj_t *object)
{
    if(!object) return;
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
    uint32_t child_count = lv_obj_get_child_count(object);
    for(uint32_t i = 0; i < child_count; ++i) {
        enable_event_bubble(lv_obj_get_child(object, (int32_t)i));
    }
}

static bool object_is_descendant_of(const lv_obj_t *object, const lv_obj_t *ancestor)
{
    while(object) {
        if(object == ancestor) return true;
        object = lv_obj_get_parent(object);
    }
    return false;
}

static void reset_close_drag(ui_control_center_t *center)
{
    if(!center) return;
    center->close_drag_indev = NULL;
    center->close_drag_tracking = false;
    center->close_dragging = false;
    center->close_drag_flick = false;
    center->close_drag_start_x = 0;
    center->close_drag_start_y = 0;
    center->close_drag_origin_y = CONTROL_CENTER_PANEL_TOP;
    center->close_drag_distance = 0;
}

static bool close_drag_pointer(ui_control_center_t *center, lv_event_t *event,
                               lv_indev_t **indev_out, lv_point_t *point_out)
{
    if(!center || !event || !indev_out || !point_out || !center->panel ||
       !lv_obj_is_valid(center->panel)) return false;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return false;
    lv_indev_get_point(indev, point_out);
    *indev_out = indev;
    return true;
}

static void close_drag_pressed(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    lv_indev_t *indev = NULL;
    lv_point_t point;
    if(center) center->suppress_click = false;
    reset_close_drag(center);
    if(!center || !center->active || !center->app || center->close_after_animation ||
       center->close_delete_timer ||
       !close_drag_pointer(center, event, &indev, &point)) return;

    lv_obj_t *target = lv_event_get_target(event);
    if(target != center->root &&
       !object_is_descendant_of(target, center->close_button)) {
        restore_settled_panel(center);
    }
    if(object_is_descendant_of(target, center->brightness) ||
       object_is_descendant_of(target, center->volume)) {
        return;
    }

    center->close_drag_indev = indev;
    center->close_drag_tracking = true;
    center->close_drag_start_x = point.x;
    center->close_drag_start_y = point.y;
    center->close_drag_origin_y = lv_obj_get_y(center->panel);
}

static void close_drag_pressing(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    lv_indev_t *indev = NULL;
    lv_point_t point;
    if(!center || !center->close_drag_tracking ||
       !close_drag_pointer(center, event, &indev, &point) ||
       indev != center->close_drag_indev) return;

    int32_t distance = center->close_drag_start_y - point.y;
    if(!center->close_dragging) {
        const int32_t horizontal = abs(point.x - center->close_drag_start_x);
        if(distance < 0 && -distance >= CONTROL_CENTER_DRAG_TRIGGER) {
            reset_close_drag(center);
            return;
        }
        if(horizontal > distance && horizontal >= CONTROL_CENTER_DRAG_TRIGGER) {
            reset_close_drag(center);
            return;
        }
        if(distance < CONTROL_CENTER_DRAG_TRIGGER) return;

        clear_gesture_pressed_state();
        lv_anim_delete(center, NULL);
        center->close_after_animation = false;
        center->close_drag_origin_y = lv_obj_get_y(center->panel);
        begin_cached_motion(center);
        center->drag_backdrop_locked = true;
        center->close_dragging = true;
        center->suppress_click = true;
        ui_app_set_control_center_drag_active(center->app, true);
    }

    if(distance < 0) distance = 0;
    int32_t maximum = center->close_drag_origin_y - CONTROL_CENTER_PANEL_HIDDEN_Y;
    if(distance > maximum) distance = maximum;
    center->close_drag_distance = distance;

    lv_point_t vector;
    lv_indev_get_vect(indev, &vector);
    center->close_drag_flick = vector.y <= -CONTROL_CENTER_FLICK_VELOCITY;
    panel_y_exec(center, center->close_drag_origin_y - distance);
}

static void close_drag_finish(ui_control_center_t *center)
{
    if(!center) return;
    if(!center->close_dragging || !center->panel || !lv_obj_is_valid(center->panel)) {
        reset_close_drag(center);
        return;
    }

    lv_obj_update_layout(center->panel);
    int32_t current = lv_obj_get_y(center->panel);
    if(current < CONTROL_CENTER_PANEL_HIDDEN_Y) current = CONTROL_CENTER_PANEL_HIDDEN_Y;
    if(current > CONTROL_CENTER_PANEL_TOP) current = CONTROL_CENTER_PANEL_TOP;
    const bool close = center->close_drag_distance >= CONTROL_CENTER_CLOSE_THRESHOLD ||
                       center->close_drag_flick;
    const int32_t target = close ? CONTROL_CENTER_PANEL_OFFSCREEN_Y :
                                   CONTROL_CENTER_PANEL_TOP;
    int32_t remaining = close ? current - target : target - current;
    if(remaining < 0) remaining = -remaining;
    uint32_t duration = (uint32_t)(100 + remaining / 3);
    if(duration < 120U) duration = 120U;
    if(duration > 240U) duration = 240U;

    center->drag_backdrop_locked = false;
    animate_panel_to(center, current, target, duration, close);
    ui_app_set_control_center_drag_active(center->app, false);
    reset_close_drag(center);
}

static void close_drag_released(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    lv_indev_t *indev = event ? lv_event_get_indev(event) : NULL;
    if(!center || !center->close_drag_tracking ||
       (indev && center->close_drag_indev && indev != center->close_drag_indev)) {
        return;
    }
    close_drag_finish(center);
}

static void close_drag_press_lost(lv_event_t *event)
{
    lv_indev_t *indev = event ? lv_event_get_indev(event) : NULL;
    if(indev && lv_indev_get_state(indev) != LV_INDEV_STATE_RELEASED) return;
    close_drag_released(event);
}

static void control_clicked(lv_event_t *event)
{
    control_binding_t *binding = lv_event_get_user_data(event);
    if(!binding || !binding->center || binding->center->suppress_click ||
       event_was_dragged(event)) return;
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    if(binding->control == APP_CONTROL_WIFI) {
        ui_app_close_control_center(binding->center->app);
        ui_app_navigate(binding->center->app, UI_ROUTE_WIFI);
        return;
    }
    if(binding->control == APP_CONTROL_BLUETOOTH) {
        ui_app_t *app = binding->center->app;
        if(!binbot_bluetooth_manager_request(app->bluetooth, BINBOT_BLUETOOTH_POWER, NULL,
                                              !app_state_model(app->state)->bluetooth))
            ui_app_show_toast(app, "蓝牙暂不可用或正在处理中");
        return;
    }
    app_state_toggle_control(binding->center->app->state, binding->control);
}

static void focus_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || !center->app || center->suppress_click || event_was_dragged(event)) return;
    if(lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    app_state_toggle_pomodoro(center->app->state);
}

static void apply_brightness(ui_control_center_t *center, uint8_t value)
{
    if(platform_set_brightness(value)) {
        app_state_set_brightness(center->app->state, value);
    }
    else {
        ui_app_show_toast(center->app, "亮度调节不可用");
    }
}

static void brightness_changed(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || !center->app) return;
    int32_t raw_value = lv_slider_get_value(center->brightness);
    uint8_t value = raw_value <= APP_BRIGHTNESS_MIN
                        ? APP_BRIGHTNESS_MIN
                        : (raw_value >= APP_BRIGHTNESS_MAX
                               ? APP_BRIGHTNESS_MAX
                               : (uint8_t)raw_value);
    if(value != raw_value) {
        /* Keep the visual control and the persisted value at the same
         * endpoint after LVGL quantizes the touch coordinate. */
        lv_slider_set_value(center->brightness, value, LV_ANIM_OFF);
    }
    apply_brightness(center, value);
}

static bool brightness_touch_at_min_end(ui_control_center_t *center, lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return false;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(center->brightness, &area);
    return point.y >= area.y2 - BRIGHTNESS_MIN_SNAP_PIXELS;
}

static void brightness_interaction(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || !center->app || !brightness_touch_at_min_end(center, event)) return;

    if(lv_slider_get_value(center->brightness) != APP_BRIGHTNESS_MIN) {
        lv_slider_set_value(center->brightness, APP_BRIGHTNESS_MIN, LV_ANIM_OFF);
        apply_brightness(center, APP_BRIGHTNESS_MIN);
    }
}

static void volume_changed(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || !center->app) return;
    uint8_t value = (uint8_t)lv_slider_get_value(center->volume);
    /* Keep the requested value in the model even during an audio reconnect;
     * main.c retries the current value when the IPC client is available. */
    app_state_set_volume(center->app->state, value);
    if(center->app->audio_client) {
        (void)binbot_audio_client_set_volume(center->app->audio_client, value);
    }
}

static void music_previous_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(center && center->app && !center->suppress_click && !event_was_dragged(event)) {
        ui_music_previous(center->app);
    }
}

static void music_play_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(center && center->app && !center->suppress_click && !event_was_dragged(event)) {
        ui_music_toggle_playback(center->app);
    }
}

static void music_next_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(center && center->app && !center->suppress_click && !event_was_dragged(event)) {
        ui_music_next(center->app);
    }
}

static lv_obj_t *create_toggle(ui_control_center_t *center, size_t index,
                               int32_t x, int32_t y, const char *icon_name,
                               const char *label)
{
    lv_obj_t *button = ui_ref_button(center->panel, x, y, 201, 155, 27, "control_tile");
    center->toggle_circles[index] = ui_box(button, 18, 18, 50, 50, 0xffffff, 25);
    center->toggle_icons[index] = ui_ref_icon(center->toggle_circles[index], icon_name, 22, UI_REF_INK);
    lv_obj_center(center->toggle_icons[index]);
    center->toggle_labels[index] = ui_ref_label(button, label, 16, UI_FONT_SEMIBOLD,
                                                UI_REF_INK, 18, 97);
    center->toggle_status[index] = ui_ref_label(button, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 18, 122);
    lv_obj_set_width(center->toggle_status[index], 165);
    lv_label_set_long_mode(center->toggle_status[index], LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(button, control_clicked, LV_EVENT_CLICKED, &center->bindings[index]);
    center->toggles[index] = button;
    return button;
}

static void create_focus_action(ui_control_center_t *center)
{
    center->focus_button = ui_ref_button(center->panel, 237, 235, 201, 155, 27, "control_tile");
    center->focus_circle = ui_box(center->focus_button, 18, 18, 50, 50, 0xe7e7e7, 25);
    ui_ref_surface(center->focus_circle, "soft50");
    center->focus_icon = ui_ref_icon(center->focus_circle, "focus", 22, UI_REF_INK);
    lv_obj_center(center->focus_icon);
    center->focus_label = ui_ref_label(center->focus_button, "专注", 16, UI_FONT_SEMIBOLD, UI_REF_INK, 18, 97);
    center->focus_status = ui_ref_label(center->focus_button, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 18, 122);
    lv_obj_set_width(center->focus_status, 165);
    lv_label_set_long_mode(center->focus_status, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(center->focus_button, focus_clicked, LV_EVENT_CLICKED, center);
}

static void music_cover_clicked(lv_event_t *event)
{
    ui_control_center_t *center = lv_event_get_user_data(event);
    if(!center || center->suppress_click || event_was_dragged(event) ||
       lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    ui_app_close_control_center(center->app);
    ui_app_navigate(center->app, UI_ROUTE_MUSIC);
}

static void create_music_controls(ui_control_center_t *center)
{
    lv_obj_t *card = ui_box(center->panel, 22, 404, 632, 93, 0xffffff, 27);
    ui_ref_surface(card, "control_music");
    lv_obj_t *cover = ui_ref_button(card, 14, 14, 65, 65, 20, "control_cover");
    center->music_cover_index = ui_ref_label(cover, "01", 11, UI_FONT_SEMIBOLD, 0xffffff, 7, 42);
    lv_obj_add_event_cb(cover, music_cover_clicked, LV_EVENT_CLICKED, center);
    center->music_title = ui_ref_label(card, "", 17, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 12);
    lv_obj_set_width(center->music_title, 300);
    lv_label_set_long_mode(center->music_title, LV_LABEL_LONG_DOT);
    center->music_artist = ui_ref_label(card, "", 12, UI_FONT_REGULAR, UI_REF_MUTED, 93, 39);
    lv_obj_set_width(center->music_artist, 300);
    lv_label_set_long_mode(center->music_artist, LV_LABEL_LONG_DOT);
    center->music_status = ui_ref_label(card, "", 11, UI_FONT_REGULAR, UI_REF_FAINT, 93, 59);
    lv_obj_t *previous = ui_ref_button(card, 407, 22, 50, 50, 25, "soft50");
    lv_obj_t *icon = ui_ref_icon(previous, "prev", 18, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(previous, music_previous_clicked, LV_EVENT_CLICKED, center);
    lv_obj_t *play = ui_button(card, 471, 16, 61, 61, UI_REF_CLAY, 31);
    lv_obj_set_style_shadow_color(play, lv_color_hex(UI_REF_CLAY), 0);
    lv_obj_set_style_shadow_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(play, 26, 0);
    lv_obj_set_style_shadow_spread(play, -8, 0);
    lv_obj_set_style_shadow_offset_y(play, 12, 0);
    center->music_play_icon = ui_ref_icon(play, "play", 25, 0xffffff);
    lv_obj_center(center->music_play_icon);
    lv_obj_add_event_cb(play, music_play_clicked, LV_EVENT_CLICKED, center);
    lv_obj_t *next = ui_ref_button(card, 546, 22, 50, 50, 25, "soft50");
    icon = ui_ref_icon(next, "next", 18, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(next, music_next_clicked, LV_EVENT_CLICKED, center);
}

static lv_obj_t *create_vertical_slider(ui_control_center_t *center, int32_t x,
                                        const char *icon_name, const char *label,
                                        lv_event_cb_t callback, lv_obj_t **value,
                                        lv_obj_t **fill)
{
    lv_obj_t *slider = lv_slider_create(center->panel);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, x, 66);
    lv_obj_set_size(slider, 94, 295);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_radius(slider, 29, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 29, LV_PART_INDICATOR);
    lv_obj_set_style_clip_corner(slider, true, 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xdcdcdc), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    /* The design has a flat fill edge, clipped by the rounded slider track. */
    *fill = ui_box(slider, 0, 295, 94, 0, 0xffffff, 0);
    lv_obj_t *icon = ui_ref_icon(slider, icon_name, 23, UI_REF_INK);
    lv_obj_set_pos(icon, 35, 231);
    *value = ui_ref_label(slider, "", 13, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 258);
    lv_obj_set_width(*value, 94);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *caption = ui_ref_label(center->panel, label, 12, UI_FONT_REGULAR, UI_REF_MUTED, x, 372);
    lv_obj_set_width(caption, 94);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, center);
    return slider;
}

static void control_wifi_timer(lv_timer_t *timer)
{
    ui_control_center_t *center = lv_timer_get_user_data(timer);
    if(center->active) ui_control_center_update(center, app_state_model(center->app->state), APP_CHANGE_CONTROLS);
}

static ui_control_center_t *control_center_create(ui_app_t *app)
{
    ui_control_center_t *center = calloc(1, sizeof(*center));
    if(!center) return NULL;
    center->app = app;
    center->active = true;
    center->root = ui_box(app->root, 0, 0, UI_WIDTH, UI_HEIGHT, 0xe9e9e9, 0);
    lv_obj_add_flag(center->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(center->root, LV_OPA_50, 0);
    center->backdrop = ui_ref_backdrop(center->root, app->page_layer, 0xe9e9e9, LV_OPA_50, 24);
    lv_obj_move_foreground(center->root);
    lv_obj_add_event_cb(center->root, close_clicked, LV_EVENT_CLICKED, center);
    lv_obj_add_event_cb(center->root, close_drag_pressed, LV_EVENT_PRESSED, center);
    lv_obj_add_event_cb(center->root, close_drag_pressing, LV_EVENT_PRESSING, center);
    lv_obj_add_event_cb(center->root, close_drag_released, LV_EVENT_RELEASED, center);
    lv_obj_add_event_cb(center->root, close_drag_press_lost, LV_EVENT_PRESS_LOST, center);
    lv_obj_remove_flag(center->root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    center->panel = ui_box(center->root, 22, CONTROL_CENTER_PANEL_TOP, 676,
                           CONTROL_CENTER_PANEL_HEIGHT, 0xf3f3f3, 36);
    ui_ref_surface(center->panel, "control_panel");
    ui_ref_label(center->panel, "控制中心", 20, UI_FONT_SEMIBOLD, UI_REF_INK, 26, 22);
    ui_ref_label(center->panel, "CONTROL CENTER", 11, UI_FONT_REGULAR, UI_REF_FAINT, 117, 31);
    ui_ref_label(center->panel, "快速调整", 12, UI_FONT_REGULAR, UI_REF_FAINT, 602, 29);
    center->bindings[0] = (control_binding_t){center, APP_CONTROL_WIFI};
    center->bindings[1] = (control_binding_t){center, APP_CONTROL_BLUETOOTH};
    center->bindings[2] = (control_binding_t){center, APP_CONTROL_DND};
    create_toggle(center, 0, 22, 66, "wifi", "无线网");
    create_toggle(center, 1, 237, 66, "bt", "蓝牙");
    create_toggle(center, 2, 22, 235, "moon", "勿扰");
    create_focus_action(center);
    center->brightness = create_vertical_slider(center, 452, "sun", "亮度", brightness_changed,
                                                 &center->brightness_value, &center->brightness_fill);
    lv_slider_set_range(center->brightness, APP_BRIGHTNESS_MIN, APP_BRIGHTNESS_MAX);
    lv_obj_add_event_cb(center->brightness, brightness_interaction, LV_EVENT_PRESSING, center);
    lv_obj_add_event_cb(center->brightness, brightness_interaction, LV_EVENT_RELEASED, center);
    center->volume = create_vertical_slider(center, 560, "speaker", "音量", volume_changed,
                                             &center->volume_value, &center->volume_fill);
    create_music_controls(center);
    lv_obj_t *close = ui_button(center->panel, 246, 515, 185, 50, 0xffffff, 25);
    center->close_button = close;
    lv_obj_set_style_shadow_color(close, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(close, 64, 0);
    lv_obj_set_style_shadow_width(close, 20, 0);
    lv_obj_set_style_shadow_spread(close, -10, 0);
    lv_obj_set_style_shadow_offset_y(close, 8, 0);
    lv_obj_t *up = ui_ref_icon(close, "back", 16, UI_REF_INK);
    lv_image_set_rotation(lv_obj_get_child(up, 0), 900);
    lv_obj_set_pos(up, 29, 17);
    ui_ref_label(close, "向上收起控制中心", 13, UI_FONT_MEDIUM, UI_REF_INK, 52, 15);
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, center);
    center->wifi = binbot_wifi_manager_create();
    if(center->wifi) (void)binbot_wifi_manager_scan(center->wifi);
    center->wifi_timer = lv_timer_create(control_wifi_timer, 1000, center);
    enable_event_bubble(center->root);
    ui_control_center_update(center, app_state_model(app->state), APP_CHANGE_ALL);
    ui_control_center_music_update(center);
    return center;
}

ui_control_center_t *ui_control_center_open(ui_app_t *app)
{
    ui_control_center_t *center = control_center_create(app);
    if(center) animate_panel_in(center);
    return center;
}

ui_control_center_t *ui_control_center_open_drag(ui_app_t *app, int32_t distance)
{
    ui_control_center_t *center = control_center_create(app);
    if(center) ui_control_center_set_drag(center, distance);
    return center;
}

void ui_control_center_prepare(ui_app_t *app)
{
    if(!app || app->control_center || app->content_frozen || app->control_center_edge_tracking) return;
    ui_control_center_t *center = control_center_create(app);
    if(!center) return;
    begin_cached_motion(center);
    ui_control_center_deactivate(center);
    app->control_center = center;
}

bool ui_control_center_is_active(const ui_control_center_t *center)
{
    return center && center->active;
}

static void prepare_reopen(ui_control_center_t *center)
{
    if(!center || !center->root || !lv_obj_is_valid(center->root)) return;
    if(center->close_delete_timer) {
        lv_timer_delete(center->close_delete_timer);
        center->close_delete_timer = NULL;
    }
    reset_close_drag(center);
    center->close_after_animation = false;
    center->drag_backdrop_locked = false;
    center->suppress_click = false;
    center->active = true;
    if(!background_is_current(center)) {
        center->background_ready = false;
        if(center->backdrop) lv_obj_delete(center->backdrop);
        center->backdrop = ui_ref_backdrop(center->root, center->app->page_layer, 0xe9e9e9, LV_OPA_50, 24);
    }
    lv_obj_set_style_bg_opa(center->root, LV_OPA_50, 0);
    if(center->backdrop) lv_obj_set_style_image_opa(center->backdrop, LV_OPA_COVER, 0);
    lv_obj_add_flag(center->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(center->root);
    ui_control_center_update(center, app_state_model(center->app->state), APP_CHANGE_ALL);
    ui_control_center_music_update(center);
}

void ui_control_center_reopen(ui_control_center_t *center)
{
    if(!center || center->active) return;
    prepare_reopen(center);
    animate_panel_in(center);
}

void ui_control_center_reopen_drag(ui_control_center_t *center, int32_t distance)
{
    if(!center || center->active) return;
    prepare_reopen(center);
    /* A newly created edge-drag overlay starts with this stable backdrop.
     * Restore the same state when reusing a parked overlay. */
    lv_obj_set_style_bg_opa(center->root, LV_OPA_50, 0);
    ui_control_center_set_drag(center, distance);
}

void ui_control_center_deactivate(ui_control_center_t *center)
{
    if(!center || !center->active) return;
    if(center->close_delete_timer) {
        lv_timer_delete(center->close_delete_timer);
        center->close_delete_timer = NULL;
    }
    lv_anim_delete(center, NULL);
    end_cached_motion(center, false);
    if(center->motion_background)
        lv_obj_add_flag(center->motion_background, LV_OBJ_FLAG_HIDDEN);
    center->close_after_animation = false;
    center->drag_backdrop_locked = false;
    reset_close_drag(center);
    center->suppress_click = false;

    /* For an animated close these values are already in place, so parking
     * changes no pixels.  Direct closes still become fully transparent and
     * move the panel beyond its shadow extent. */
    panel_y_exec(center, CONTROL_CENTER_PANEL_OFFSCREEN_Y);
    lv_obj_remove_flag(center->root, LV_OBJ_FLAG_CLICKABLE);
    center->active = false;
}

void ui_control_center_set_drag(ui_control_center_t *center, int32_t distance)
{
    if(!center || !center->active || !center->panel ||
       !lv_obj_is_valid(center->panel)) return;
    if(distance < 0) distance = 0;
    if(distance > CONTROL_CENTER_PANEL_TRAVEL) distance = CONTROL_CENTER_PANEL_TRAVEL;
    center->close_after_animation = false;
    lv_anim_delete(center, NULL);
    /* Keep the backdrop at the opacity assigned when the overlay was
     * created.  This makes the underlying page a stable composition while
     * the panel itself is moved for every touch sample. */
    center->drag_backdrop_locked = true;
    begin_cached_motion(center);
    panel_y_exec(center, CONTROL_CENTER_PANEL_HIDDEN_Y + distance);
}

void ui_control_center_finish_drag(ui_control_center_t *center, int32_t distance)
{
    if(!center || !center->active || !center->panel ||
       !lv_obj_is_valid(center->panel)) return;
    if(distance < 0) distance = 0;
    if(distance > CONTROL_CENTER_PANEL_TRAVEL) distance = CONTROL_CENTER_PANEL_TRAVEL;
    lv_obj_update_layout(center->panel);
    int32_t current = lv_obj_get_y(center->panel);
    if(current < CONTROL_CENTER_PANEL_HIDDEN_Y) current = CONTROL_CENTER_PANEL_HIDDEN_Y;
    if(current > CONTROL_CENTER_PANEL_TOP) current = CONTROL_CENTER_PANEL_TOP;

    const bool open = distance >= CONTROL_CENTER_OPEN_THRESHOLD;
    const int32_t target = open ? CONTROL_CENTER_PANEL_TOP :
                                  CONTROL_CENTER_PANEL_OFFSCREEN_Y;
    const int32_t remaining = open ? target - current : current - target;
    uint32_t duration = (uint32_t)(100 + (remaining > 0 ? remaining / 3 : 0));
    if(duration < 120U) duration = 120U;
    if(duration > 240U) duration = 240U;
    center->drag_backdrop_locked = false;
    animate_panel_to(center, current, target, duration, !open);
}

void ui_control_center_update(ui_control_center_t *center, const app_model_t *model,
                              uint32_t changes)
{
    if(!center || !model) return;
    bool controls_changed = changes == APP_CHANGE_ALL || (changes & APP_CHANGE_CONTROLS);
    bool focus_changed = changes == APP_CHANGE_ALL ||
                         (changes & (APP_CHANGE_POMODORO |
                                     APP_CHANGE_POMODORO_PROGRESS |
                                     APP_CHANGE_POMODORO_PHASE));
    if(!controls_changed && !focus_changed) return;

    if(controls_changed) {
        bool active[] = {model->wifi, model->bluetooth, model->dnd};
        char ssid[BINBOT_WIFI_SSID_MAX] = "";
        if(center->wifi)
            (void)binbot_wifi_manager_snapshot(center->wifi, NULL, 0, NULL, NULL,
                                                ssid, sizeof(ssid), NULL, 0);
        for(size_t i = 0; i < CONTROL_TOGGLE_COUNT; ++i) {
            lv_obj_set_style_bg_image_opa(center->toggles[i], active[i] ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(center->toggles[i],
                                      lv_color_hex(active[i] ? UI_REF_CLAY : 0xffffff), 0);
            lv_obj_set_style_shadow_color(center->toggles[i], lv_color_hex(active[i] ? UI_REF_CLAY : 0), 0);
            lv_obj_set_style_shadow_opa(center->toggles[i], active[i] ? LV_OPA_COVER : 31, 0);
            lv_obj_set_style_shadow_offset_y(center->toggles[i], active[i] ? 14 : 10, 0);
            lv_obj_set_style_bg_image_opa(center->toggle_circles[i], active[i] ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(center->toggle_circles[i], lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_opa(center->toggle_circles[i], active[i] ? LV_OPA_20 : LV_OPA_COVER, 0);
            if(!active[i]) ui_ref_surface(center->toggle_circles[i], "soft50");
            uint32_t foreground = active[i] ? 0xffffff : UI_REF_INK;
            ui_icon_set_color(center->toggle_icons[i], foreground);
            lv_obj_set_style_text_color(center->toggle_labels[i], lv_color_hex(foreground), 0);
            lv_obj_set_style_text_color(center->toggle_status[i],
                                        lv_color_hex(active[i] ? 0xf9caba : UI_REF_FAINT), 0);
            const char *status = active[i] ? "已开启" : "已关闭";
            if(i == 0 && active[i]) status = ssid[0] ? ssid : "未连接";
            lv_label_set_text(center->toggle_status[i], status);
        }
        uint8_t brightness = model->brightness < APP_BRIGHTNESS_MIN
                                 ? APP_BRIGHTNESS_MIN
                                 : model->brightness;
        lv_slider_set_value(center->brightness, brightness, LV_ANIM_OFF);
        lv_slider_set_value(center->volume, model->volume, LV_ANIM_OFF);
        int32_t height = 295 * brightness / 100;
        lv_obj_set_y(center->brightness_fill, 295 - height);
        lv_obj_set_height(center->brightness_fill, height);
        height = 295 * model->volume / 100;
        lv_obj_set_y(center->volume_fill, 295 - height);
        lv_obj_set_height(center->volume_fill, height);
        char value[12];
        snprintf(value, sizeof(value), "%u%%", brightness);
        lv_label_set_text(center->brightness_value, value);
        snprintf(value, sizeof(value), "%u%%", model->volume);
        lv_label_set_text(center->volume_value, value);
    }

    if(focus_changed) {
        const bool running = model->pomodoro.running;
        const uint32_t background = running ? UI_REF_CLAY : 0xffffff;
        const uint32_t foreground = running ? 0xffffff : UI_REF_INK;
        const uint32_t status_color = running ? 0xf9caba : UI_REF_FAINT;
        lv_obj_set_style_bg_image_opa(center->focus_button, running ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(center->focus_button, lv_color_hex(background), 0);
        lv_obj_set_style_shadow_color(center->focus_button, lv_color_hex(running ? UI_REF_CLAY : 0), 0);
        lv_obj_set_style_shadow_opa(center->focus_button, running ? LV_OPA_COVER : 31, 0);
        lv_obj_set_style_shadow_offset_y(center->focus_button, running ? 14 : 10, 0);
        lv_obj_set_style_bg_image_opa(center->focus_circle, running ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(center->focus_circle, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(center->focus_circle, running ? LV_OPA_20 : LV_OPA_COVER, 0);
        ui_icon_set_color(center->focus_icon, foreground);
        lv_obj_set_style_text_color(center->focus_label, lv_color_hex(foreground), 0);

        char status[32];
        const char *prefix = model->pomodoro.mode == APP_POMODORO_BREAK
                                 ? (running ? "休息剩余 " : "继续休息 ")
                                 : (running ? "剩余 " : "继续专注 ");
        if(!running && model->pomodoro.mode == APP_POMODORO_FOCUS &&
           model->pomodoro.seconds_left == model->pomodoro.focus_seconds) {
            prefix = "开始番茄钟 ";
        }
        snprintf(status, sizeof(status), "%s%02lu:%02lu", prefix,
                 (unsigned long)(model->pomodoro.seconds_left / 60U),
                 (unsigned long)(model->pomodoro.seconds_left % 60U));
        lv_label_set_text(center->focus_status, status);
        lv_obj_set_style_text_color(center->focus_status, lv_color_hex(status_color), 0);
    }
    if(center->motion_settled && !panel_snapshot_is_current(center))
        restore_settled_panel(center);
}

void ui_control_center_music_update(ui_control_center_t *center)
{
    if(!center || !center->app) return;
    const ui_music_track_t *track = ui_music_current_track(center->app);
    const ui_music_state_t *music = &center->app->music;
    if(center->music_title) lv_label_set_text(center->music_title, track->title);
    if(center->music_artist) lv_label_set_text(center->music_artist, track->artist);
    if(center->music_play_icon) {
        ui_ref_icon_set_name(center->music_play_icon, music->playing ? "pause" : "play");
    }
    char index[12];
    snprintf(index, sizeof(index), "%02lu", (unsigned long)music->track_index + 1);
    lv_label_set_text(center->music_cover_index, index);
    if(center->music_status) {
        lv_label_set_text(center->music_status, music->playing ? "正在播放" : "已暂停");
        lv_obj_set_style_text_color(center->music_status,
                                    lv_color_hex(music->playing ? UI_REF_CLAY : UI_REF_FAINT), 0);
    }
    if(center->motion_settled && !panel_snapshot_is_current(center))
        restore_settled_panel(center);
}

void ui_control_center_destroy(ui_control_center_t *center)
{
    if(!center) return;
    if(center->wifi_timer) lv_timer_delete(center->wifi_timer);
    if(center->wifi) binbot_wifi_manager_destroy(center->wifi);
    if(center->close_delete_timer) {
        lv_timer_delete(center->close_delete_timer);
        center->close_delete_timer = NULL;
    }
    reset_close_drag(center);
    if(center->panel && lv_obj_is_valid(center->panel)) lv_anim_delete(center, NULL);
    if(center->root && lv_obj_is_valid(center->root)) lv_obj_delete(center->root);
    if(center->background_snapshot) {
        lv_image_cache_drop(center->background_snapshot);
        lv_draw_buf_destroy(center->background_snapshot);
    }
    if(center->panel_snapshot) {
        lv_image_cache_drop(center->panel_snapshot);
        lv_draw_buf_destroy(center->panel_snapshot);
    }
    free(center);
}
