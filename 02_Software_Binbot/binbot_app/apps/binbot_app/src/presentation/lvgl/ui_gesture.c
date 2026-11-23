#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { POPUP_WAIT, POPUP_EXECUTING, POPUP_DONE, POPUP_CANCEL, POPUP_ERROR } popup_phase_t;
enum { AUTO_DELAY_MS = 2200, RESULT_DELAY_MS = 1300, CONFIRM_DELAY_MS = 15000 };

struct ui_gesture_control {
    ui_app_t *app;
    lv_timer_t *timer;
    lv_display_t *display;
    void *capture;
    uint64_t sequence, last_frame_ms, retry_ms, popup_until, phase_started;
    binbot_gesture_stabilizer_t stabilizer;
    lv_obj_t *popup, *panel, *progress;
    popup_phase_t phase;
    const char *detail;
    app_gesture_id_t pending_gesture;
    app_gesture_action_t pending_action;
    bool waiting_release, photo_pending, ask, phase_visible;
    uint8_t release_seen;
    uint32_t photo_started;
};

static const char *names[] = {"OK", "点赞", "张开手掌", "比耶", "握拳"};
static const char *icons[] = {"gesture_ok", "gesture_thumb", "gesture_palm", "gesture_v", "gesture_fist"};
static const char *actions[] = {"打开电脑", "电脑休眠", "开关台灯", "播放 / 暂停", "下一首", "上一首",
                                "拍照", "开始专注", "唤醒小伴", "白噪音", "静音", "不执行"};
static const char *action_icons[] = {"monitor", "moon", "bulb", "play", "next", "prev",
                                     "camera", "focus", "spark", "wave", "speaker", "x"};

const char *ui_gesture_name(app_gesture_id_t g) { return (unsigned)g < APP_GESTURE_COUNT ? names[g] : ""; }
const char *ui_gesture_icon(app_gesture_id_t g) { return (unsigned)g < APP_GESTURE_COUNT ? icons[g] : "gesture_ok"; }
const char *ui_gesture_action_name(app_gesture_action_t a) { return (unsigned)a < APP_GESTURE_ACTION_COUNT ? actions[a] : ""; }
const char *ui_gesture_action_icon(app_gesture_action_t a) { return (unsigned)a < APP_GESTURE_ACTION_COUNT ? action_icons[a] : "x"; }

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void close_popup(ui_gesture_control_t *c)
{
    if(c->popup) {
        lv_obj_add_flag(c->popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(c->popup);
    }
    c->popup = c->panel = c->progress = NULL;
    c->phase_visible = false;
}

static const char *execute(ui_gesture_control_t *c, app_gesture_action_t action)
{
    ui_app_t *app = c->app;
    switch(action) {
    case APP_GESTURE_PC_ON:
    case APP_GESTURE_PC_SLEEP:
        return "电脑控制服务尚未接入";
    case APP_GESTURE_LAMP:
        return "台灯控制服务尚未接入";
    case APP_GESTURE_PLAY: {
        if(!app->audio_client) return "音频服务未连接";
        bool playing = app->music.playing;
        ui_music_toggle_playback(app);
        if(playing == app->music.playing) return "播放状态未改变，请检查音频服务";
        break;
    }
    case APP_GESTURE_NEXT:
    case APP_GESTURE_PREVIOUS:
        if(!app->audio_client) return "音频服务未连接";
        if(action == APP_GESTURE_NEXT) ui_music_next(app);
        else ui_music_previous(app);
        break;
    case APP_GESTURE_PHOTO:
        c->photo_pending = true;
        c->photo_started = platform_monotonic_ms();
        if(app->route != UI_ROUTE_CAMERA) ui_app_navigate(app, UI_ROUTE_CAMERA);
        break;
    case APP_GESTURE_FOCUS:
        app_state_set_pomodoro_mode(app->state, APP_POMODORO_FOCUS);
        app_state_start_pomodoro(app->state);
        ui_app_navigate(app, UI_ROUTE_POMODORO);
        break;
    case APP_GESTURE_WAKE:
        if(!ui_app_request_wake(app)) return "语音服务未连接";
        break;
    case APP_GESTURE_NOISE: {
        binbot_audio_noise_state_t noise = app->noise;
        noise.playing = !noise.playing;
        if(!app->audio_client || !ui_noise_apply(app, &noise))
            return "音频服务未连接";
        else ui_app_navigate(app, UI_ROUTE_NOISE);
        break;
    }
    case APP_GESTURE_MUTE:
        if(app->audio_client && binbot_audio_client_set_volume(app->audio_client, 0)) {
            app_state_set_volume(app->state, 0);
        }
        else return "音频服务未连接";
        break;
    default: break;
    }
    return NULL;
}

static bool allowed(ui_gesture_control_t *c, app_gesture_id_t g, app_gesture_action_t action)
{
    const app_gesture_settings_t *s = &app_state_model(c->app->state)->gestures;
    return s->enabled && (s->enabled_mask & (1U << g)) &&
           s->actions[g] == action && action != APP_GESTURE_NOTHING;
}

static void render_popup(ui_gesture_control_t *c);

static void set_phase(ui_gesture_control_t *c, popup_phase_t phase, const char *detail)
{
    c->phase = phase;
    c->detail = detail;
    c->phase_visible = false;
    if(c->popup) render_popup(c);
}

static void execute_pending(ui_gesture_control_t *c)
{
    if(!allowed(c, c->pending_gesture, c->pending_action)) {
        set_phase(c, POPUP_CANCEL, "这次不执行任何操作");
        return;
    }
    const char *error = execute(c, c->pending_action);
    if(error) set_phase(c, POPUP_ERROR, error);
    else if(c->photo_pending) set_phase(c, POPUP_EXECUTING, "正在等待相机画面");
    else set_phase(c, POPUP_DONE, "操作已执行");
}

static void confirm(lv_event_t *event)
{
    ui_gesture_control_t *c = lv_event_get_user_data(event);
    if(!c->popup || c->phase != POPUP_WAIT) return;
    if(c->phase_visible && monotonic_ms() >= c->popup_until) set_phase(c, POPUP_CANCEL, "确认已超时");
    else execute_pending(c);
}

static void cancel(lv_event_t *event)
{
    ui_gesture_control_t *c = lv_event_get_user_data(event);
    if(c->popup && c->phase == POPUP_WAIT)
        set_phase(c, POPUP_CANCEL, "这次不执行任何操作");
}

static void pulse_ring(void *object, int32_t value)
{
    int32_t size = 104 + value * 94 / 1000;
    lv_obj_set_size(object, size, size);
    lv_obj_set_pos(object, 220 - size / 2, 80 - size / 2);
    lv_obj_set_style_opa(object, 51 * (1000 - value) / 1000, 0);
}

static void render_popup(ui_gesture_control_t *c)
{
    if(c->panel) {
        lv_obj_add_flag(c->panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(c->panel);
    }
    c->progress = NULL;
    bool waiting = c->phase == POPUP_WAIT;
    int32_t height = waiting && !c->ask ? 357 : 365;
    lv_obj_t *panel = ui_box(c->popup, 140, (720 - height) / 2, 440, height, 0xfbfaf7, 34);
    c->panel = panel;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    ui_ref_shadow(panel, 0, 115, 70, -20, 30);
    if(waiting) {
        for(unsigned i = 0; i < 2; ++i) {
            lv_obj_t *ring = ui_box(panel, 168, 28, 104, 104, 0, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_opa(ring, 0, 0);
            lv_obj_set_style_border_color(ring, lv_color_hex(UI_REF_CLAY), 0);
            lv_obj_set_style_border_width(ring, 2, 0);
            lv_obj_set_style_opa(ring, 102, 0);
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, ring);
            lv_anim_set_exec_cb(&animation, pulse_ring);
            lv_anim_set_values(&animation, 0, 1000);
            lv_anim_set_duration(&animation, 1800);
            lv_anim_set_delay(&animation, i * 900);
            lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&animation);
        }
    }
    bool done = c->phase == POPUP_DONE;
    uint32_t color = done ? 0x10b981 : waiting || c->phase == POPUP_EXECUTING ? UI_REF_CLAY : 0xe7e7e7;
    lv_obj_t *disc = ui_box(panel, 176, 36, 88, 88, color, LV_RADIUS_CIRCLE);
    if(waiting) ui_ref_shadow(disc, UI_REF_CLAY, 255, 30, -10, 14);
    lv_obj_t *icon = ui_ref_icon(disc, done ? "check" : icons[c->pending_gesture], done ? 40 : 46,
                                done || waiting || c->phase == POPUP_EXECUTING ? 0xffffff : UI_REF_FAINT);
    lv_obj_center(icon);
    lv_obj_t *label = ui_ref_label(panel, "识别到手势", 15, UI_FONT_REGULAR, UI_REF_FAINT, 0, 148);
    lv_obj_set_width(label, 440);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    char text[96]; snprintf(text, sizeof(text), "「%s」", names[c->pending_gesture]);
    label = ui_ref_label(panel, text, 26, UI_FONT_SEMIBOLD, UI_REF_INK, 0, 173);
    lv_obj_set_width(label, 440);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    if(c->phase == POPUP_CANCEL) snprintf(text, sizeof(text), "已取消");
    else if(c->phase == POPUP_ERROR) snprintf(text, sizeof(text), "未执行");
    else if(c->phase == POPUP_EXECUTING) snprintf(text, sizeof(text), "正在拍照");
    else snprintf(text, sizeof(text), done ? "已%s" : c->ask ? "是否%s？" : "即将%s", actions[c->pending_action]);
    lv_obj_t *badge = ui_box(panel, 0, 217, LV_SIZE_CONTENT, 44, 0xe7e7e7, 22);
    lv_obj_set_style_pad_hor(badge, 16, 0);
    lv_obj_set_style_pad_column(badge, 8, 0);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_ref_icon(badge, action_icons[c->pending_action], 18, UI_REF_INK);
    ui_ref_label(badge, text, 17, UI_FONT_MEDIUM, UI_REF_INK, 0, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 217);
    if(waiting && c->ask) {
        lv_obj_t *button = ui_button(panel, 24, 285, 190, 56, 0xe7e7e7, 28);
        ui_centered_label(button, "取消", ui_ref_font(17, UI_FONT_MEDIUM), UI_REF_INK);
        lv_obj_add_event_cb(button, cancel, LV_EVENT_CLICKED, c);
        button = ui_button(panel, 226, 285, 190, 56, UI_REF_CLAY, 28);
        ui_centered_label(button, "确认", ui_ref_font(17, UI_FONT_SEMIBOLD), 0xffffff);
        lv_obj_add_event_cb(button, confirm, LV_EVENT_CLICKED, c);
    }
    else if(waiting) {
        lv_obj_t *track = ui_box(panel, 24, 305, 308, 8, 0xf5f5f5, 4);
        c->progress = ui_box(track, 0, 0, 1, 8, UI_REF_CLAY, 4);
        lv_obj_t *button = ui_button(panel, 344, 285, 72, 48, 0xe7e7e7, 24);
        ui_centered_label(button, "取消", ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
        lv_obj_add_event_cb(button, cancel, LV_EVENT_CLICKED, c);
    }
    else {
        label = ui_ref_label(panel, c->detail, 15, UI_FONT_REGULAR, UI_REF_FAINT, 24, 297);
        lv_obj_set_width(label, 392);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

bool ui_gesture_popup_active(const ui_gesture_control_t *c) { return c && c->popup; }

void ui_gesture_dismiss_popup(ui_gesture_control_t *c)
{
    if(c) close_popup(c);
}

void ui_gesture_popup_raise(ui_gesture_control_t *c)
{
    if(!c || !c->popup) return;
    lv_obj_remove_flag(c->popup, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    if(c->panel) lv_obj_remove_flag(c->panel, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    if(lv_obj_get_child(c->app->root, -1) != c->popup) lv_obj_move_foreground(c->popup);
}

void ui_gesture_try(ui_app_t *app, app_gesture_id_t g)
{
    if(!app || !app->gestures || (unsigned)g >= APP_GESTURE_COUNT) return;
    ui_gesture_control_t *c = app->gestures;
    const app_gesture_settings_t *s = &app_state_model(app->state)->gestures;
    if(!s->enabled) { ui_app_show_toast(app, "手势识别已关闭"); return; }
    app_gesture_action_t action = s->actions[g];
    if(!allowed(c, g, action)) { ui_app_show_toast(app, "该手势未启用"); return; }
    if(c->popup || c->photo_pending) return;
    c->pending_gesture = g; c->pending_action = action;
    c->ask = action <= APP_GESTURE_PC_SLEEP && s->confirm_sensitive;
    c->phase = POPUP_WAIT;
    c->phase_visible = false;
    ui_screensaver_wake(app->screensaver);
    ui_app_set_control_center_drag_active(app, false);
    ui_app_close_control_center(app);
    lv_obj_t *overlay = ui_box(app->root, 0, 0, 720, 720, 0, 0);
    c->popup = overlay;
    lv_obj_set_style_bg_opa(overlay, 0, 0);
    if(!ui_ref_backdrop(overlay, app->root, 0, 51, 3)) lv_obj_set_style_bg_opa(overlay, 51, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, cancel, LV_EVENT_CLICKED, c);
    render_popup(c);
    ui_gesture_popup_raise(c);
}

void ui_gesture_receive(ui_app_t *app, const binbot_hand_result_t *result)
{
    if(!app || !app->gestures || !result) return;
    ui_gesture_control_t *c = app->gestures;
    uint64_t now = monotonic_ms();
    if(!app_state_model(app->state)->gestures.enabled || result->sequence == 0 ||
       result->sequence <= c->sequence || result->monotonic_ms > now ||
       now - result->monotonic_ms > 800 || result->monotonic_ms < c->last_frame_ms) return;
    c->sequence = result->sequence;
    if(c->last_frame_ms && result->monotonic_ms - c->last_frame_ms > 1000) {
        c->stabilizer.candidate_frames = 0;
        c->stabilizer.absent_frames = 0;
    }
    c->last_frame_ms = result->monotonic_ms;
    binbot_gesture_label_t changed;
    bool transition = binbot_gesture_stabilizer_update(&c->stabilizer, &result->gesture, &changed);
    if(c->waiting_release) {
        if(result->gesture.label == BINBOT_GESTURE_NONE) {
            if(++c->release_seen >= 3) c->waiting_release = false;
        }
        else c->release_seen = 0;
        return;
    }
    if(!transition || changed == BINBOT_GESTURE_NONE) return;
    app_gesture_id_t g;
    switch(changed) {
    case BINBOT_GESTURE_OK: g = APP_GESTURE_OK; break;
    case BINBOT_GESTURE_THUMB_UP: g = APP_GESTURE_THUMB; break;
    case BINBOT_GESTURE_OPEN_PALM: g = APP_GESTURE_PALM; break;
    case BINBOT_GESTURE_VICTORY: g = APP_GESTURE_VICTORY; break;
    case BINBOT_GESTURE_CLOSED_FIST: g = APP_GESTURE_FIST; break;
    default: return;
    }
    fprintf(stdout, "gestures: recognized %s score=%.3f\n", names[g], result->gesture.score);
    if(allowed(c, g, app_state_model(app->state)->gestures.actions[g])) ui_gesture_try(app, g);
}

void ui_gesture_before_route(ui_gesture_control_t *c, bool release_capture)
{
    if(!c) return;
    if(release_capture) {
        ui_camera_gestures_stop(c->capture); c->capture = NULL;
        c->sequence = 0;
        c->retry_ms = 0;
    }
}

void ui_gesture_config_changed(ui_gesture_control_t *c)
{
    if(!c) return;
    close_popup(c);
    c->photo_pending = false;
    ui_camera_gestures_changed(c->app);
    const app_gesture_settings_t *s = &app_state_model(c->app->state)->gestures;
    const uint8_t frames[] = {8, 6, 4};
    const float scores[] = {0.80f, 0.65f, 0.50f};
    binbot_gesture_stabilizer_config_t config = {frames[s->sensitivity], 3, scores[s->sensitivity]};
    binbot_gesture_stabilizer_init(&c->stabilizer, &config);
    c->waiting_release = true;
    c->release_seen = 0;
    if(!s->enabled) {
        ui_camera_gestures_stop(c->capture); c->capture = NULL;
        c->sequence = 0; c->photo_pending = false;
    }
}

static void tick(lv_timer_t *timer)
{
    ui_gesture_control_t *c = lv_timer_get_user_data(timer);
    ui_app_t *app = c->app;
    uint64_t now = monotonic_ms();
    if(c->popup) {
        ui_gesture_popup_raise(c);
        if(c->phase_visible && c->phase == POPUP_WAIT && !c->ask && c->progress) {
            uint64_t elapsed = now - c->phase_started;
            lv_obj_set_width(c->progress, 1 + 307 * (elapsed < AUTO_DELAY_MS ? elapsed : AUTO_DELAY_MS) / AUTO_DELAY_MS);
        }
        if(c->phase_visible && c->phase != POPUP_EXECUTING && now >= c->popup_until) {
            if(c->phase == POPUP_WAIT && !c->ask) execute_pending(c);
            else if(c->phase == POPUP_WAIT) set_phase(c, POPUP_CANCEL, "确认已超时");
            else close_popup(c);
        }
    }
    if(c->photo_pending && !app->route_pending) {
        if(app->route != UI_ROUTE_CAMERA || platform_monotonic_ms() - c->photo_started > 5000) {
            c->photo_pending = false;
            set_phase(c, POPUP_ERROR, "相机不可用，未拍摄照片");
        }
        else if(ui_camera_take_photo(app)) {
            c->photo_pending = false;
            set_phase(c, POPUP_DONE, "照片已保存");
        }
    }
    const char *capture = getenv("BINBOT_GESTURE_CAPTURE");
    if(!app_state_model(app->state)->gestures.enabled || app->route == UI_ROUTE_CAMERA ||
       app->route_pending || (capture && !strcmp(capture, "0"))) return;
    if(!c->capture && now >= c->retry_ms) {
        c->retry_ms = now + 10000;
        c->capture = ui_camera_gestures_start();
        c->sequence = 0;
    }
    binbot_hand_result_t result;
    if(c->capture && ui_camera_gestures_latest(c->capture, c->sequence, &result))
        ui_gesture_receive(app, &result);
}

static void refresh_ready(lv_event_t *event)
{
    ui_gesture_control_t *c = lv_event_get_user_data(event);
    if(!c->popup || c->phase_visible) return;
    /* Count the visible time, including on devices where the first frame is slow. */
    c->phase_visible = true;
    c->phase_started = monotonic_ms();
    c->popup_until = c->phase_started + (c->phase == POPUP_WAIT ?
                    (c->ask ? CONFIRM_DELAY_MS : AUTO_DELAY_MS) : RESULT_DELAY_MS);
}

ui_gesture_control_t *ui_gesture_control_create(ui_app_t *app)
{
    ui_gesture_control_t *c = calloc(1, sizeof(*c));
    if(!c) return NULL;
    c->app = app;
    c->display = lv_obj_get_display(app->root);
    ui_gesture_config_changed(c);
    c->waiting_release = false;
    c->timer = lv_timer_create(tick, 100, c);
    lv_display_add_event_cb(c->display, refresh_ready, LV_EVENT_REFR_READY, c);
    return c;
}

void ui_gesture_control_destroy(ui_gesture_control_t *c)
{
    if(!c) return;
    lv_display_remove_event_cb_with_user_data(c->display, refresh_ready, c);
    lv_timer_delete(c->timer);
    close_popup(c);
    ui_gesture_before_route(c, true);
    free(c);
}
