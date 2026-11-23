#ifndef BINBOT_UI_INTERNAL_H
#define BINBOT_UI_INTERNAL_H

#include "binbot/app_state.h"
#include "binbot/app_calendar.h"
#include "binbot/ambient_display.h"
#include "binbot/audio/client.h"
#include "binbot/bluetooth_manager.h"
#include "binbot/platform.h"
#include "binbot/ui_app.h"
#include "binbot/vision/hand_detector.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    UI_WIDTH = 720,
    UI_HEIGHT = 720,
    UI_STATUS_HEIGHT = 40,
    UI_PAGE_HEIGHT = 680,
    UI_CONTROL_CENTER_DRAG_MAX = 740,
    UI_ROUTE_HISTORY_MAX = 32,
};

typedef enum {
    UI_ROUTE_HOME,
    UI_ROUTE_APPS,
    UI_ROUTE_MUSIC,
    UI_ROUTE_ALARM,
    UI_ROUTE_POMODORO,
    UI_ROUTE_VOCABULARY,
    UI_ROUTE_TODO,
    UI_ROUTE_DEVICES,
    UI_ROUTE_ASSISTANT,
    UI_ROUTE_CAMERA,
    UI_ROUTE_SETTINGS,
    UI_ROUTE_WIFI,
    UI_ROUTE_LUNCH,
    UI_ROUTE_MODULES,
    UI_ROUTE_ABOUT,
    UI_ROUTE_BLUETOOTH,
    UI_ROUTE_CALENDAR,
    UI_ROUTE_CALENDAR_DETAIL,
    UI_ROUTE_NOISE,
    UI_ROUTE_DIAGNOSTICS,
    UI_ROUTE_GESTURES,
    UI_ROUTE_SCREENSAVER_SETTINGS,
} ui_route_t;

typedef struct ui_screen {
    lv_obj_t *root;
    void *context;
    void (*update)(void *context, const app_model_t *model, uint32_t changes);
    void (*destroy)(void *context);
    void (*set_paused)(void *context, bool paused);
} ui_screen_t;

typedef struct ui_control_center ui_control_center_t;
typedef struct ui_module_notice ui_module_notice_t;
typedef struct ui_calendar_notice ui_calendar_notice_t;
typedef struct ui_chat_session ui_chat_session_t;
typedef struct ui_screensaver ui_screensaver_t;
typedef struct ui_gesture_control ui_gesture_control_t;

enum {
    UI_MUSIC_TRACK_COUNT = 3,
};

typedef struct {
    const char *title;
    const char *artist;
    uint32_t duration_seconds;
    const char *path;
} ui_music_track_t;

typedef struct {
    size_t track_index;
    uint32_t position_seconds;
    bool active;
    bool playing;
    bool liked;
    bool repeating;
} ui_music_state_t;

typedef enum {
    UI_ICON_ALARM_CLOCK,
    UI_ICON_BATTERY_FULL,
    UI_ICON_BELL,
    UI_ICON_BLUETOOTH,
    UI_ICON_BRAIN,
    UI_ICON_CAMERA,
    UI_ICON_CHECK,
    UI_ICON_CHEVRON_DOWN,
    UI_ICON_CHEVRON_LEFT,
    UI_ICON_CHEVRON_RIGHT,
    UI_ICON_CLOUD_SUN,
    UI_ICON_COFFEE,
    UI_ICON_DROPLETS,
    UI_ICON_FLOWER_2,
    UI_ICON_HEART,
    UI_ICON_HAND,
    UI_ICON_LEAF,
    UI_ICON_LIGHTBULB,
    UI_ICON_LIST_TODO,
    UI_ICON_MIC,
    UI_ICON_MONITOR,
    UI_ICON_MOON,
    UI_ICON_LIST_MUSIC,
    UI_ICON_MORE_HORIZONTAL,
    UI_ICON_PAUSE,
    UI_ICON_PLAY,
    UI_ICON_PLUS,
    UI_ICON_POWER,
    UI_ICON_ROTATE_CCW,
    UI_ICON_REPEAT_2,
    UI_ICON_SIGNAL,
    UI_ICON_SETTINGS,
    UI_ICON_SPARKLES,
    UI_ICON_SPEAKER,
    UI_ICON_SKIP_BACK,
    UI_ICON_SKIP_FORWARD,
    UI_ICON_SUN,
    UI_ICON_THERMOMETER,
    UI_ICON_TIMER,
    UI_ICON_VOLUME_2,
    UI_ICON_WIFI,
    UI_ICON_WIND,
    UI_ICON_ZAP,
    UI_ICON_COUNT,
} ui_icon_name_t;

typedef enum {
    UI_FONT_REGULAR,
    UI_FONT_MEDIUM,
    UI_FONT_SEMIBOLD,
    UI_FONT_BOLD,
    UI_FONT_EXTRABOLD,
} ui_font_weight_t;

struct ui_app {
    app_state_t *state;
    app_modules_t *modules;
    app_firmware_t firmware;
    binbot_audio_client_t *audio_client;
    binbot_bluetooth_manager_t *bluetooth;
    binbot_slave_client_t *slave_client;
    lv_obj_t *root;
    lv_obj_t *status_bar;
    lv_obj_t *page_layer;
    lv_obj_t *status_time;
    lv_obj_t *status_signal;
    lv_obj_t *status_wifi;
    lv_obj_t *status_battery;
    lv_timer_t *clock_timer;
    lv_timer_t *music_timer;
    ui_music_state_t music;
    binbot_audio_noise_state_t noise;
    ui_chat_session_t *chat;
    app_calendar_t calendar;
    time_t calendar_day;
    char calendar_selected[64];
    bool calendar_folded;
    bool calendar_from_home;
    int32_t calendar_scroll_y;
    int32_t calendar_detail_scroll_y;
    bool calendar_scroll_valid;
    ui_screen_t current;
    ui_route_t route;
    ui_route_t pending_route;
    bool route_pending;
    ui_route_t route_history[UI_ROUTE_HISTORY_MAX];
    size_t route_history_len;
    bool pending_route_is_back;
    size_t pending_back_history_len;
    bool voice_listening;
    int voice_state;
    /* The slave reports input state changes, but some firmware revisions can
     * emit the same decoded key more than once while an ADC value settles.
     * Keep a small edge latch here so one physical press maps to one action. */
    uint8_t last_slave_adc_key;
    bool last_slave_encoder_down;
    bool slave_input_latched;
    int32_t last_encoder_count;
    bool encoder_count_valid;
    ui_control_center_t *control_center;
    ui_module_notice_t *module_notice;
    ui_calendar_notice_t *calendar_notice;
    ui_screensaver_t *screensaver;
    ui_gesture_control_t *gestures;
    /* Freeze the underlying page while a control-center edge drag is active.
     * The model continues to advance; the page is synchronized on release. */
    bool content_frozen;
    uint32_t frozen_changes;
    /* Edge swipe state is kept at the app level so it survives route changes
     * and works for every screen tree. */
    lv_indev_t *control_center_indev;
    bool control_center_edge_tracking;
    bool control_center_dragging;
    int32_t control_center_start_x;
    int32_t control_center_start_y;
};

enum {
    UI_COLOR_BACKGROUND = 0xf4f5f7,
    UI_COLOR_TEXT = 0x171a22,
    UI_COLOR_SECONDARY = 0x777b88,
    UI_COLOR_CARD = 0xffffff,
    UI_COLOR_ALARM = 0xff375f,
    UI_COLOR_POMODORO = 0xff9500,
    UI_COLOR_VOCABULARY = 0x0a84ff,
    UI_COLOR_TODO = 0x30b0c7,
    UI_COLOR_DEVICES = 0x5e5ce6,
    UI_COLOR_ASSISTANT = 0x0a84ff,
    UI_COLOR_MUSIC = 0x0a84ff,
};

const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_body_large(void);
const lv_font_t *ui_font_status(void);
const lv_font_t *ui_font_number(void);
const lv_font_t *ui_font_number_large(void);
const lv_font_t *ui_font_clock(void);
const lv_font_t *ui_font_icon(void);
const lv_font_t *ui_font_phonetic(void);
const lv_font_t *ui_font_text_px(uint16_t size, ui_font_weight_t weight);
const lv_font_t *ui_font_display_px(uint16_t size, ui_font_weight_t weight);
void ui_theme_initialize(void);
void ui_theme_shutdown(void);
/* Borrowed font results must be attached with set_font/retain_font before the
 * next LVGL turn. Widget deletion releases the loan automatically. */
const lv_font_t *ui_resources_font(const char *path, const char *fallback_path, uint16_t size);
void ui_resources_set_font(lv_obj_t *object, const lv_font_t *font, lv_style_selector_t selector);
bool ui_resources_retain_font(lv_obj_t *owner, const lv_font_t *font);
void ui_resources_collect(void);
size_t ui_resources_font_count(void);
void ui_resources_shutdown(void);
lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
                 uint32_t color, int32_t radius);
lv_obj_t *ui_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
                    uint32_t color, int32_t radius);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, int32_t x, int32_t y);
lv_obj_t *ui_centered_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color);
lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_name_t name, int32_t size, uint32_t color);
void ui_icon_set_name(lv_obj_t *icon, ui_icon_name_t name);
void ui_icon_set_color(lv_obj_t *icon, uint32_t color);
void ui_icon_set_visible(lv_obj_t *icon, bool visible);
void ui_add_soft_shadow(lv_obj_t *object, uint32_t color, lv_opa_t opacity, int32_t width,
                        int32_t offset_y);
void ui_format_time(char *buffer, size_t size);
void ui_format_date(char *buffer, size_t size);
lv_obj_t *ui_create_app_shell(ui_app_t *app, const char *title, uint32_t accent,
                              lv_obj_t **content);

enum {
    UI_REF_INK = 0x1a1a1a,
    UI_REF_CLAY = 0xe8643c,
    UI_REF_MUTED = 0x737373,
    UI_REF_FAINT = 0xa3a3a3,
};
const lv_font_t *ui_ref_font(uint16_t size, ui_font_weight_t weight);
void ui_ref_surface(lv_obj_t *object, const char *name);
void ui_ref_shadow(lv_obj_t *object, uint32_t color, lv_opa_t opacity,
                    int32_t width, int32_t spread, int32_t offset_y);
lv_obj_t *ui_ref_backdrop(lv_obj_t *parent, lv_obj_t *source, uint32_t tint,
                         uint8_t opacity, uint8_t blur_radius);
lv_obj_t *ui_ref_label(lv_obj_t *parent, const char *text, uint16_t size,
                       ui_font_weight_t weight, uint32_t color, int32_t x, int32_t y);
lv_obj_t *ui_ref_icon(lv_obj_t *parent, const char *name, int32_t size, uint32_t color);
void ui_ref_icon_set_name(lv_obj_t *icon, const char *name);
lv_obj_t *ui_ref_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t width,
                        int32_t height, int32_t radius, const char *surface);
lv_obj_t *ui_ref_shell(ui_app_t *app, const char *title, const char *en);
lv_obj_t *ui_ref_shell_latest(ui_app_t *app, const char *title, const char *en);
lv_obj_t *ui_ref_page(ui_app_t *app);
lv_obj_t *ui_ref_ring(lv_obj_t *parent, int32_t x, int32_t y, int32_t size,
                      int32_t width);

void ui_app_navigate(ui_app_t *app, ui_route_t route);
void ui_app_go_back(ui_app_t *app);
ui_gesture_control_t *ui_gesture_control_create(ui_app_t *app);
void ui_gesture_control_destroy(ui_gesture_control_t *control);
void ui_gesture_before_route(ui_gesture_control_t *control, bool release_capture);
void ui_gesture_config_changed(ui_gesture_control_t *control);
void ui_gesture_receive(ui_app_t *app, const binbot_hand_result_t *result);
void ui_gesture_try(ui_app_t *app, app_gesture_id_t gesture);
bool ui_gesture_popup_active(const ui_gesture_control_t *control);
void ui_gesture_dismiss_popup(ui_gesture_control_t *control);
void ui_gesture_popup_raise(ui_gesture_control_t *control);
void ui_camera_gestures_changed(ui_app_t *app);
const char *ui_gesture_name(app_gesture_id_t gesture);
const char *ui_gesture_icon(app_gesture_id_t gesture);
const char *ui_gesture_action_name(app_gesture_action_t action);
const char *ui_gesture_action_icon(app_gesture_action_t action);
void *ui_camera_gestures_start(void);
void ui_camera_gestures_stop(void *capture);
bool ui_camera_gestures_latest(void *capture, uint64_t after_sequence, binbot_hand_result_t *result);
bool ui_camera_take_photo(ui_app_t *app);
ui_screensaver_t *ui_screensaver_create(ui_app_t *app);
void ui_screensaver_show(ui_screensaver_t *saver);
bool ui_screensaver_preview(ui_screensaver_t *saver, const char *theme_id);
void ui_screensaver_snapshot(ui_screensaver_t *saver, binbot_ambient_snapshot_t *data);
binbot_ambient_display_t *ui_screensaver_display(ui_screensaver_t *saver);
void ui_screensaver_wake(ui_screensaver_t *saver);
bool ui_screensaver_active(const ui_screensaver_t *saver);
void ui_screensaver_raise(ui_screensaver_t *saver);
void ui_screensaver_destroy(ui_screensaver_t *saver);
ui_module_notice_t *ui_module_notice_create(ui_app_t *app);
void ui_module_notice_raise(ui_module_notice_t *notice);
void ui_module_notice_destroy(ui_module_notice_t *notice);
ui_calendar_notice_t *ui_calendar_notice_create(ui_app_t *app);
void ui_calendar_notice_show(ui_calendar_notice_t *notice, const app_calendar_changes_t *changes);
void ui_calendar_notice_raise(ui_calendar_notice_t *notice);
void ui_calendar_notice_destroy(ui_calendar_notice_t *notice);
void ui_app_rebuild(ui_app_t *app);
void ui_app_open_control_center(ui_app_t *app);
bool ui_app_open_control_center_drag(ui_app_t *app, int32_t distance);
void ui_app_update_control_center_drag(ui_app_t *app, int32_t distance);
void ui_app_finish_control_center_drag(ui_app_t *app, int32_t distance);
void ui_app_set_control_center_drag_active(ui_app_t *app, bool active);
void ui_app_close_control_center(ui_app_t *app);
void ui_app_show_toast(ui_app_t *app, const char *message);
void ui_app_set_audio_client(ui_app_t *app, binbot_audio_client_t *client);
void ui_app_handle_music_state(ui_app_t *app, int state,
                               uint32_t position_ms, uint32_t duration_ms);

size_t ui_music_track_count(void);
const ui_music_track_t *ui_music_track_at(size_t index);
const ui_music_track_t *ui_music_current_track(const ui_app_t *app);
void ui_music_toggle_playback(ui_app_t *app);
void ui_music_previous(ui_app_t *app);
void ui_music_next(ui_app_t *app);
void ui_music_select_track(ui_app_t *app, size_t index);
void ui_music_seek(ui_app_t *app, uint32_t position_seconds);
void ui_music_toggle_repeat(ui_app_t *app);
void ui_music_toggle_like(ui_app_t *app);
void ui_music_screen_sync(void *context);
bool ui_noise_apply(ui_app_t *app, const binbot_audio_noise_state_t *state);
void ui_noise_screen_sync(void *context);
ui_screen_t ui_noise_screen_create(ui_app_t *app);

ui_screen_t ui_home_screen_create(ui_app_t *app);
ui_screen_t ui_apps_screen_create(ui_app_t *app);
void ui_apps_screen_rotate(ui_app_t *app, int32_t steps);
ui_screen_t ui_music_screen_create(ui_app_t *app);
ui_screen_t ui_alarm_screen_create(ui_app_t *app);
ui_screen_t ui_pomodoro_screen_create(ui_app_t *app);
ui_screen_t ui_vocabulary_screen_create(ui_app_t *app);
ui_screen_t ui_todo_screen_create(ui_app_t *app);
ui_screen_t ui_calendar_screen_create(ui_app_t *app);
ui_screen_t ui_calendar_detail_screen_create(ui_app_t *app);
void ui_calendar_open_event(ui_app_t *app, const char *id, bool from_home);
ui_screen_t ui_devices_screen_create(ui_app_t *app);
ui_screen_t ui_assistant_screen_create(ui_app_t *app);
void ui_assistant_screen_sync(void *context);
void ui_chat_session_destroy(ui_chat_session_t *session);
void ui_chat_reset_playback(ui_app_t *app);
typedef struct {
    unsigned revision;
    int state;
    char question[BINBOT_DIALOG_TEXT_MAX + 1];
    char reply[BINBOT_DIALOG_TEXT_MAX + 1];
} ui_chat_preview_t;
void ui_chat_get_preview(ui_app_t *app, ui_chat_preview_t *preview);
const char *ui_chat_status_text(int state);
ui_screen_t ui_camera_screen_create(ui_app_t *app);
ui_screen_t ui_lunch_screen_create(ui_app_t *app);
ui_screen_t ui_bluetooth_screen_create(ui_app_t *app);
ui_screen_t ui_settings_screen_create(ui_app_t *app);
ui_screen_t ui_screensaver_settings_screen_create(ui_app_t *app);
ui_screen_t ui_gestures_screen_create(ui_app_t *app);
ui_screen_t ui_about_screen_create(ui_app_t *app);
ui_screen_t ui_diagnostics_screen_create(ui_app_t *app);
ui_screen_t ui_modules_screen_create(ui_app_t *app);
ui_screen_t ui_wifi_screen_create(ui_app_t *app);

ui_control_center_t *ui_control_center_open(ui_app_t *app);
void ui_control_center_prepare(ui_app_t *app);
ui_control_center_t *ui_control_center_open_drag(ui_app_t *app, int32_t distance);
bool ui_control_center_is_active(const ui_control_center_t *center);
void ui_control_center_reopen(ui_control_center_t *center);
void ui_control_center_reopen_drag(ui_control_center_t *center, int32_t distance);
void ui_control_center_deactivate(ui_control_center_t *center);
void ui_control_center_set_drag(ui_control_center_t *center, int32_t distance);
void ui_control_center_finish_drag(ui_control_center_t *center, int32_t distance);
void ui_control_center_update(ui_control_center_t *center, const app_model_t *model,
                              uint32_t changes);
void ui_control_center_music_update(ui_control_center_t *center);
void ui_control_center_destroy(ui_control_center_t *center);

#endif
