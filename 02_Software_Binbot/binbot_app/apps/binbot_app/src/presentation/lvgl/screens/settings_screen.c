#include "../ui_internal.h"
#include "binbot/wifi_manager.h"
#include "binbot/build_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ui_app_t *app;
    lv_obj_t *volume;
    lv_obj_t *brightness;
    lv_obj_t *volume_value;
    lv_obj_t *brightness_value;
    lv_obj_t *wifi_status;
    lv_obj_t *wifi_dot;
    lv_obj_t *bluetooth_status;
    lv_obj_t *bluetooth_dot;
    lv_obj_t *gesture_summary;
    lv_obj_t *standby_summary;
    binbot_wifi_manager_t *wifi;
    lv_timer_t *wifi_timer;
} settings_context_t;

static void settings_unavailable(lv_event_t *event)
{
    ui_app_show_toast(lv_event_get_user_data(event), "该设置尚未接入设备服务");
}

static void settings_wifi_clicked(lv_event_t *event)
{
    ui_app_navigate(lv_event_get_user_data(event), UI_ROUTE_WIFI);
}

static void settings_bluetooth_clicked(lv_event_t *event)
{
    ui_app_navigate(lv_event_get_user_data(event), UI_ROUTE_BLUETOOTH);
}

static void settings_about_clicked(lv_event_t *event)
{
    ui_app_navigate(lv_event_get_user_data(event), UI_ROUTE_ABOUT);
}

static void settings_gestures_clicked(lv_event_t *event)
{
    ui_app_navigate(lv_event_get_user_data(event), UI_ROUTE_GESTURES);
}

static void settings_screensaver_clicked(lv_event_t *event)
{
    ui_app_navigate(lv_event_get_user_data(event), UI_ROUTE_SCREENSAVER_SETTINGS);
}

static void settings_slider_changed(lv_event_t *event)
{
    settings_context_t *context = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    if(slider == context->brightness) {
        if(platform_set_brightness(value)) app_state_set_brightness(context->app->state, value);
        else ui_app_show_toast(context->app, "亮度调节不可用");
    }
    else {
        app_state_set_volume(context->app->state, value);
        if(context->app->audio_client)
            (void)binbot_audio_client_set_volume(context->app->audio_client, value);
    }
}

static void settings_network_sync(settings_context_t *context, const app_model_t *model)
{
    char ssid[BINBOT_WIFI_SSID_MAX] = "";
    binbot_wifi_state_t state;
    if(context->wifi)
        (void)binbot_wifi_manager_snapshot(context->wifi, NULL, 0, NULL, &state,
                                            ssid, sizeof(ssid), NULL, 0);
    char text[BINBOT_WIFI_SSID_MAX + 32];
    snprintf(text, sizeof(text), "%s%s", model->wifi ? (ssid[0] ? ssid : "未连接") : "已关闭",
             model->wifi && ssid[0] ? " · 已连接" : "");
    lv_label_set_text(context->wifi_status, text);
    lv_obj_set_style_bg_color(context->wifi_dot,
                              lv_color_hex(model->wifi && ssid[0] ? 0x10b981 : 0xd4d4d4), 0);
    binbot_bluetooth_snapshot_t bluetooth;
    if(binbot_bluetooth_manager_snapshot(context->app->bluetooth, &bluetooth)) {
        const char *status = !bluetooth.ready ? "正在读取…" : !bluetooth.available ? "不可用" :
                               !bluetooth.powered ? "已关闭" : "未连接";
        bool connected = false;
        for(size_t i = 0; i < bluetooth.device_count; ++i) {
            if(bluetooth.powered && bluetooth.devices[i].connected) {
                status = bluetooth.devices[i].name; connected = true; break;
            }
        }
        lv_label_set_text(context->bluetooth_status, status);
        lv_obj_set_style_bg_color(context->bluetooth_dot,
                                  lv_color_hex(connected ? 0x10b981 : 0xd4d4d4), 0);
    }
}

static void settings_network_timer(lv_timer_t *timer)
{
    settings_context_t *context = lv_timer_get_user_data(timer);
    if(context->app->content_frozen) return;
    settings_network_sync(context, app_state_model(context->app->state));
}

static void settings_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    settings_context_t *context = user_data;
    if(!context || !model) return;
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_SCREENSAVER)) {
        const app_screensaver_settings_t *s = &model->screensaver;
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_find(
            ui_screensaver_display(context->app->screensaver), s->theme_id);
        char summary[128], delay[32];
        if(s->delay_seconds < 60) snprintf(delay, sizeof(delay), "%u 秒", s->delay_seconds);
        else snprintf(delay, sizeof(delay), "%u 分钟", s->delay_seconds / 60);
        if(!s->delay_seconds) snprintf(summary, sizeof(summary), "已关闭自动进入");
        else snprintf(summary, sizeof(summary), "%s后 · %s", delay,
            !strcmp(s->theme_id, "face") ? "小伴表情" :
            s->shuffle ? "随机表盘" : theme ? theme->name : "表盘");
        lv_label_set_text(context->standby_summary, summary);
    }
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_GESTURES)) {
        const app_gesture_settings_t *s = &model->gestures;
        char summary[96];
        if(!s->enabled) snprintf(summary, sizeof(summary), "手势识别已关闭");
        else if(!(s->enabled_mask & 1U)) snprintf(summary, sizeof(summary), "OK 手势 · 未启用");
        else snprintf(summary, sizeof(summary), "OK 手势 · %s", ui_gesture_action_name(s->actions[APP_GESTURE_OK]));
        lv_label_set_text(context->gesture_summary, summary);
    }
    if(changes != APP_CHANGE_ALL && !(changes & APP_CHANGE_CONTROLS)) return;
    char value[12];
    snprintf(value, sizeof(value), "%u%%", model->volume);
    lv_label_set_text(context->volume_value, value);
    snprintf(value, sizeof(value), "%u%%", model->brightness);
    lv_label_set_text(context->brightness_value, value);
    lv_slider_set_value(context->volume, model->volume, LV_ANIM_OFF);
    lv_slider_set_value(context->brightness, model->brightness, LV_ANIM_OFF);
    settings_network_sync(context, model);
}

static lv_obj_t *settings_slider(settings_context_t *context, lv_obj_t *root,
                                  int32_t x, const char *label, const char *en,
                                  int32_t en_x, lv_obj_t **value)
{
    lv_obj_t *card = ui_box(root, x, 0, 331, 122, 0xffffff, 25);
    ui_ref_surface(card, "settings_slider_latest");
    ui_ref_label(card, label, 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 25);
    ui_ref_label(card, en, 15, UI_FONT_REGULAR, UI_REF_FAINT, en_x, 27);
    *value = ui_ref_label(card, "", 21, UI_FONT_SEMIBOLD, UI_REF_INK, 249, 21);
    lv_obj_set_width(*value, 64);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *track = ui_box(card, 18, 60, 295, 43, 0xe0e0e0, 22);
    lv_obj_t *slider = lv_slider_create(track);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, 4, 4);
    lv_obj_set_size(slider, 287, 35);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 18, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_slider_changed, LV_EVENT_VALUE_CHANGED, context);
    return slider;
}

static void settings_destroy(void *user_data)
{
    settings_context_t *context = user_data;
    if(!context) return;
    if(context->wifi_timer) lv_timer_delete(context->wifi_timer);
    if(context->wifi) binbot_wifi_manager_destroy(context->wifi);
    free(context);
}

static void settings_paused(void *user_data, bool paused)
{
    settings_context_t *context = user_data;
    if(!context->wifi_timer) return;
    if(paused) lv_timer_pause(context->wifi_timer);
    else { lv_timer_resume(context->wifi_timer); lv_timer_ready(context->wifi_timer); }
}

ui_screen_t ui_settings_screen_create(ui_app_t *app)
{
    settings_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    lv_obj_t *root = ui_ref_shell_latest(app, "设置", "SETTINGS");
    lv_obj_t *content = ui_box(root, 0, 96, 720, 624, 0, 0);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(content, 22, 0);
    context->volume = settings_slider(context, content, 22, "音量", "Volume", 56, &context->volume_value);
    context->brightness = settings_slider(context, content, 367, "屏幕亮度", "Brightness", 90,
                                           &context->brightness_value);
    lv_slider_set_range(context->brightness, APP_BRIGHTNESS_MIN, APP_BRIGHTNESS_MAX);

    lv_obj_t *network = ui_box(content, 22, 136, 331, 88, 0xffffff, 25);
    ui_ref_surface(network, "settings_network_modules");
    ui_ref_label(network, "网络", 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 18);
    ui_ref_label(network, "Wi-Fi", 15, UI_FONT_REGULAR, UI_REF_FAINT, 56, 20);
    context->wifi_dot = ui_box(network, 18, 57, 7, 7, 0xd4d4d4, 4);
    context->wifi_status = ui_ref_label(network, "未连接", 15, UI_FONT_REGULAR,
                                        UI_REF_MUTED, 30, 49);
    lv_obj_set_width(context->wifi_status, 194);
    lv_label_set_long_mode(context->wifi_status, LV_LABEL_LONG_DOT);
    lv_obj_t *change = ui_ref_button(network, 247, 24, 66, 40, 20, "switch_network");
    ui_centered_label(change, "切换", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(change, settings_wifi_clicked, LV_EVENT_CLICKED, app);

    lv_obj_t *bluetooth = ui_box(content, 367, 136, 331, 88, 0xffffff, 25);
    ui_ref_surface(bluetooth, "settings_network_modules");
    ui_ref_label(bluetooth, "蓝牙音箱", 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 18);
    ui_ref_label(bluetooth, "Bluetooth", 15, UI_FONT_REGULAR, UI_REF_FAINT, 90, 20);
    context->bluetooth_dot = ui_box(bluetooth, 18, 57, 7, 7, 0xd4d4d4, 4);
    context->bluetooth_status = ui_ref_label(bluetooth, "未连接", 15, UI_FONT_REGULAR, UI_REF_MUTED, 30, 49);
    lv_obj_set_width(context->bluetooth_status, 194);
    lv_label_set_long_mode(context->bluetooth_status, LV_LABEL_LONG_DOT);
    lv_obj_t *manage = ui_ref_button(bluetooth, 247, 24, 66, 40, 20, "switch_network");
    ui_centered_label(manage, "管理", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(manage, settings_bluetooth_clicked, LV_EVENT_CLICKED, app);

    lv_obj_t *standby = ui_ref_button(content, 22, 238, 331, 88, 25, "settings_network_modules");
    lv_obj_t *standby_tile = ui_box(standby, 18, 24, 40, 40, UI_REF_INK, 12);
    lv_obj_t *standby_icon = ui_ref_icon(standby_tile, "moon", 19, 0xffffff);
    lv_obj_center(standby_icon);
    ui_ref_label(standby, "表盘", 17, UI_FONT_MEDIUM, UI_REF_INK, 70, 18);
    ui_ref_label(standby, "Watch Face", 15, UI_FONT_REGULAR, UI_REF_FAINT, 109, 20);
    context->standby_summary = ui_ref_label(standby, "", 15, UI_FONT_REGULAR, UI_REF_MUTED, 70, 49);
    lv_obj_set_width(context->standby_summary, 210);
    lv_label_set_long_mode(context->standby_summary, LV_LABEL_LONG_DOT);
    lv_obj_t *standby_arrow = ui_ref_icon(standby, "back", 20, UI_REF_FAINT);
    lv_obj_set_pos(standby_arrow, 293, 34);
    lv_image_set_rotation(lv_obj_get_child(standby_arrow, 0), 1800);
    lv_obj_add_event_cb(standby, settings_screensaver_clicked, LV_EVENT_CLICKED, app);

    lv_obj_t *gesture = ui_ref_button(content, 367, 238, 331, 88, 25, "settings_network_modules");
    lv_obj_t *gesture_tile = ui_box(gesture, 18, 24, 40, 40, 0xfcece6, 12);
    lv_obj_t *gesture_icon = ui_ref_icon(gesture_tile, "gesture_ok", 21, UI_REF_CLAY);
    lv_obj_center(gesture_icon);
    ui_ref_label(gesture, "手势设置", 17, UI_FONT_MEDIUM, UI_REF_INK, 70, 18);
    ui_ref_label(gesture, "Gestures", 15, UI_FONT_REGULAR, UI_REF_FAINT, 146, 20);
    context->gesture_summary = ui_ref_label(gesture, "", 15, UI_FONT_REGULAR, UI_REF_MUTED, 70, 49);
    lv_obj_set_width(context->gesture_summary, 210);
    lv_label_set_long_mode(context->gesture_summary, LV_LABEL_LONG_DOT);
    lv_obj_t *gesture_arrow = ui_ref_icon(gesture, "back", 20, UI_REF_FAINT);
    lv_obj_set_pos(gesture_arrow, 293, 34);
    lv_image_set_rotation(lv_obj_get_child(gesture_arrow, 0), 1800);
    lv_obj_add_event_cb(gesture, settings_gestures_clicked, LV_EVENT_CLICKED, app);

    lv_obj_t *voice = ui_box(content, 22, 340, 676, 76, 0xffffff, 25);
    ui_ref_surface(voice, "settings_voice_modules");
    ui_ref_label(voice, "AI 语音", 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 26);
    ui_ref_label(voice, "Voice", 15, UI_FONT_REGULAR, UI_REF_FAINT, 74, 28);
    const char *voices[] = {"温柔", "活力", "沉稳"};
    for(size_t i = 0; i < 3; ++i) {
        lv_obj_t *button = ui_button(voice, 127 + (int32_t)i * 179, 18, 172, 40,
                                       i ? 0xe6e6e6 : UI_REF_INK, 20);
        if(i == 0) lv_obj_set_style_bg_color(button, lv_color_hex(UI_REF_INK), 0);
        ui_centered_label(button, voices[i], ui_ref_font(15, UI_FONT_MEDIUM),
                            i ? UI_REF_INK : 0xffffff);
        lv_obj_add_event_cb(button, settings_unavailable, LV_EVENT_CLICKED, app);
    }

    lv_obj_t *privacy = ui_box(content, 22, 430, 676, 72, 0xffffff, 25);
    lv_obj_set_style_bg_grad_color(privacy, lv_color_hex(0xf6f6f6), 0);
    lv_obj_set_style_bg_grad_dir(privacy, LV_GRAD_DIR_VER, 0);
    ui_ref_label(privacy, "隐私", 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 24);
    ui_ref_label(privacy, "Privacy", 15, UI_FONT_REGULAR, UI_REF_FAINT, 56, 26);
    const char *labels[] = {"语音唤醒", "麦克风", "保存历史"};
    const int32_t text_x[] = {163, 359, 527};
    const int32_t toggle_x[] = {232, 415, 597};
    for(size_t i = 0; i < 3; ++i) {
        ui_ref_label(privacy, labels[i], 15, UI_FONT_REGULAR, 0x525252, text_x[i], 26);
        lv_obj_t *toggle = ui_button(privacy, toggle_x[i], 18, 61, 36,
                                      i < 2 ? UI_REF_CLAY : 0xd6d6d6, 18);
        ui_box(toggle, i < 2 ? 29 : 4, 4, 29, 29, 0xffffff, 15);
        lv_obj_add_event_cb(toggle, settings_unavailable, LV_EVENT_CLICKED, app);
    }
    lv_obj_t *about = ui_ref_button(content, 22, 516, 676, 64, 25, "settings_about_modules");
    lv_obj_t *box = ui_box(about, 18, 12, 40, 40, 0xfcece6, 12);
    lv_obj_t *spark = ui_ref_icon(box, "spark", 19, UI_REF_CLAY); lv_obj_center(spark);
    ui_ref_label(about, "关于本机", 17, UI_FONT_MEDIUM, UI_REF_INK, 70, 20);
    ui_ref_label(about, "About", 15, UI_FONT_REGULAR, UI_REF_FAINT, 142, 22);
    lv_obj_t *version = ui_ref_label(about, "v" BINBOT_APP_VERSION, 15, UI_FONT_REGULAR, UI_REF_FAINT, 518, 22);
    lv_obj_set_width(version, 106);
    lv_label_set_long_mode(version, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *arrow = ui_ref_icon(about, "back", 20, UI_REF_FAINT);
    lv_obj_set_pos(arrow, 638, 22);
    lv_image_set_rotation(lv_obj_get_child(arrow, 0), 1800);
    lv_obj_add_event_cb(about, settings_about_clicked, LV_EVENT_CLICKED, app);
    context->wifi = binbot_wifi_manager_create();
    if(context->wifi) (void)binbot_wifi_manager_scan(context->wifi);
    context->wifi_timer = lv_timer_create(settings_network_timer, 1000, context);
    settings_update(context, app_state_model(app->state), APP_CHANGE_ALL);
    return (ui_screen_t){root, context, settings_update, settings_destroy, settings_paused};
}
