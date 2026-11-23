#include "../ui_internal.h"
#include "binbot/platform.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct desk_context desk_context_t;
typedef struct { desk_context_t *context; int action; } desk_binding_t;
struct desk_context {
    ui_app_t *app;
    lv_obj_t *root, *wake_overlay;
    lv_obj_t *tip, *accept, *dismiss, *light, *value, *percent, *power, *power_icon;
    lv_obj_t *glow, *slider, *pc, *monitor, *stand, *pc_en, *pc_name, *pc_status, *pc_dot;
    lv_obj_t *pc_button, *pc_button_label;
    lv_obj_t *charge_glow, *charge_ring, *charge_phone, *charge_bolt, *charge_toggle, *charge_knob;
    lv_obj_t *charge_badge, *charge_status, *charge_dot;
    lv_timer_t *charge_timer;
    int charge;
    bool paused;
    lv_obj_t *tones[3], *tone_labels[3], *devices[3], *tiles[3], *icons[3], *names[3], *statuses[3];
    lv_obj_t *scenes[4], *scene_labels[4], *scene_icons[4];
    desk_binding_t bindings[16];
    int brightness, tone, scene, tip_action;
    app_computer_state_t previous_pc;
};

/* Keep the existing persisted device bits in their original positions. */
static const size_t indices[3] = {2, 3, 1};
static const char *names[3] = {"挂灯", "风扇", "加湿器"};
static const char *statuses[3] = {"护眼模式", "微风 1 档", "湿度 → 50%"};
static const char *icons[3] = {"lightbar", "fan", "drop"};
static const char *tones[3] = {"暖光", "自然", "冷白"};
static const uint32_t colors[3] = {0xf6b26b, 0xf7e3c4, 0xe6eefc};
static const char *scenes[4] = {"学习", "办公", "午休", "离开"};
static const char *scene_icons[4] = {"book", "briefcase", "moon", "power"};
static const char *tips[4] = {
    "台灯调到 85% 自然光，电脑也在开机了，开始学习吧。",
    "冷白光更提神，电脑正在开机，今天也加油。",
    "灯关了，20 分钟后我叫你。", "桌上的设备都关好了，电脑已锁屏。",
};

static void tip_set(desk_context_t *c, const char *text, int action)
{
    c->tip_action = action;
    lv_label_set_text(c->tip, text);
    lv_obj_set_width(c->tip, action ? 416 : 582);
    lv_obj_set_flag(c->accept, LV_OBJ_FLAG_HIDDEN, action == 0);
    lv_obj_set_flag(c->dismiss, LV_OBJ_FLAG_HIDDEN, action == 0);
    lv_label_set_text(lv_obj_get_child(c->accept, 0), action == 2 ? "休眠" : "调亮");
}

static void desk_sync(desk_context_t *c, const app_model_t *m)
{
    bool lamp = m->devices[0].enabled;
    char text[48];
    snprintf(text, sizeof(text), "光线 %s", lamp ? (c->brightness > 60 ? "充足" : "适中") : "偏暗");
    lv_label_set_text(c->light, text);
    snprintf(text, sizeof(text), "%d", lamp ? c->brightness : 0);
    lv_label_set_text(c->value, text);
    lv_obj_set_x(c->percent, lamp && c->brightness == 100 ? 81 : lamp ? 63 : 43);
    lv_obj_set_style_bg_color(c->power, lv_color_hex(lamp ? UI_REF_CLAY : 0xe6e6e6), 0);
    lv_obj_set_style_image_recolor(lv_obj_get_child(c->power_icon, 0), lv_color_hex(lamp ? 0xffffff : UI_REF_MUTED), 0);
    char glow[24];
    snprintf(glow, sizeof(glow), "lamp_glow%d", c->tone);
    ui_ref_surface(c->glow, glow);
    lv_obj_set_style_opa(c->glow, lamp ? c->brightness * 255 / 140 : 0, 0);
    lv_slider_set_value(c->slider, lamp ? c->brightness : 0, LV_ANIM_OFF);
    for(int i = 0; i < 3; ++i) {
        bool selected = i == c->tone;
        lv_obj_set_style_bg_color(c->tones[i], lv_color_hex(selected ? UI_REF_INK : 0xe6e6e6), 0);
        lv_obj_set_style_text_color(c->tone_labels[i], lv_color_hex(selected ? 0xffffff : 0x525252), 0);
        bool on = m->devices[indices[i]].enabled;
        lv_obj_set_style_bg_image_opa(c->devices[i], on ? 0 : 255, 0);
        lv_obj_set_style_border_width(c->devices[i], on ? 1 : 0, 0);
        lv_obj_set_style_bg_color(c->tiles[i], lv_color_hex(on ? UI_REF_CLAY : 0xffffff), 0);
        lv_obj_set_style_image_recolor(lv_obj_get_child(c->icons[i], 0), lv_color_hex(on ? 0xffffff : UI_REF_FAINT), 0);
        lv_obj_set_style_text_color(c->names[i], lv_color_hex(on ? UI_REF_INK : UI_REF_MUTED), 0);
        lv_label_set_text(c->statuses[i], on ? statuses[i] : "已关闭");
    }
    bool on = m->computer == APP_COMPUTER_ONLINE;
    bool waking = m->computer == APP_COMPUTER_WAKING || m->computer == APP_COMPUTER_SELECTED;
    ui_ref_surface(c->pc, on ? "desk_pc_on" : "desk_pc");
    lv_obj_set_style_bg_color(c->monitor, lv_color_hex(on ? 0xf08b63 : waking ? 0xfcece6 : 0xf5f5f5), 0);
    lv_obj_set_style_border_color(c->monitor, lv_color_hex(on ? 0xffffff : waking ? UI_REF_CLAY : 0xd4d4d4), 0);
    lv_obj_set_style_bg_color(c->stand, lv_color_hex(on ? 0xffffff : 0xd4d4d4), 0);
    lv_obj_set_style_text_color(c->pc_en, lv_color_hex(on ? 0x8c8c8c : UI_REF_FAINT), 0);
    lv_obj_set_style_text_color(c->pc_name, lv_color_hex(on ? 0xffffff : UI_REF_INK), 0);
    lv_obj_set_style_text_color(c->pc_status, lv_color_hex(waking ? UI_REF_CLAY : on ? 0xbababa : UI_REF_FAINT), 0);
    lv_label_set_text(c->pc_status, on ? "已开机 · 运行中" : waking ? "正在网络唤醒…" : "已关机");
    lv_obj_set_style_bg_color(c->pc_dot, lv_color_hex(on ? 0x7ed38f : waking ? UI_REF_CLAY : 0xd4d4d4), 0);
    lv_obj_set_style_bg_color(c->pc_button, lv_color_hex(on ? 0x313131 : UI_REF_CLAY), 0);
    lv_label_set_text(c->pc_button_label, on ? "休眠" : waking ? "开机中" : "开机");
    lv_obj_set_state(c->pc_button, LV_STATE_DISABLED, waking);
    if(on && c->previous_pc == APP_COMPUTER_WAKING)
        tip_set(c, "你的台式机已经开好了，可以直接坐下用了。", 0);
    c->previous_pc = m->computer;
    lv_obj_set_flag(c->charge_phone, LV_OBJ_FLAG_HIDDEN, c->charge != 2);
    lv_obj_set_flag(c->charge_bolt, LV_OBJ_FLAG_HIDDEN, c->charge == 2);
    lv_obj_set_flag(c->charge_ring, LV_OBJ_FLAG_HIDDEN, c->charge != 2);
    lv_obj_set_style_opa(c->charge_glow, c->charge == 2 ? 255 : 0, 0);
    lv_obj_set_style_bg_color(c->charge_toggle, lv_color_hex(c->charge ? UI_REF_CLAY : 0xd4d4d4), 0);
    lv_obj_set_x(c->charge_knob, c->charge ? 21 : 3);
    lv_label_set_text(c->charge_status, c->charge == 2 ? "充电中" : c->charge == 1 ? "等待放置" : "已关闭");
    lv_obj_set_style_text_color(c->charge_status, lv_color_hex(c->charge == 2 ? UI_REF_CLAY : UI_REF_MUTED), 0);
    lv_obj_set_style_bg_color(c->charge_badge, lv_color_hex(c->charge == 2 ? 0xfcece6 : 0xf5f5f5), 0);
    lv_obj_set_style_bg_color(c->charge_dot, lv_color_hex(c->charge == 2 ? UI_REF_CLAY : 0xa3a3a3), 0);
    for(int i = 0; i < 4; ++i) {
        bool selected = i == c->scene;
        lv_obj_set_style_bg_image_opa(c->scenes[i], selected ? 0 : 255, 0);
        lv_obj_set_style_bg_color(c->scenes[i], lv_color_hex(selected ? UI_REF_CLAY : 0xffffff), 0);
        lv_obj_set_style_text_color(c->scene_labels[i], lv_color_hex(selected ? 0xffffff : 0x404040), 0);
        lv_obj_set_style_image_recolor(lv_obj_get_child(c->scene_icons[i], 0), lv_color_hex(selected ? 0xffffff : UI_REF_CLAY), 0);
    }
}

static void wake_pc(desk_context_t *c)
{
    app_state_select_computer(c->app->state, platform_monotonic_ms());
    app_state_confirm_computer_wake(c->app->state, platform_monotonic_ms());
}

static void close_wake_dialog(desk_context_t *c)
{
    if(!c->wake_overlay) return;
    lv_obj_delete_async(c->wake_overlay);
    c->wake_overlay = NULL;
}

static void cancel_wake(lv_event_t *event)
{
    close_wake_dialog(lv_event_get_user_data(event));
}

static void confirm_wake(lv_event_t *event)
{
    desk_context_t *c = lv_event_get_user_data(event);
    if(!c->wake_overlay) return;
    close_wake_dialog(c);
    if(app_state_model(c->app->state)->computer != APP_COMPUTER_SLEEP) return;
    wake_pc(c);
    tip_set(c, "正在通过网络唤醒你的台式机，大概需要几秒钟…", 0);
}

static void open_wake_dialog(desk_context_t *c)
{
    if(c->wake_overlay) return;
    c->wake_overlay = ui_box(c->root, 0, 0, UI_WIDTH, UI_HEIGHT, 0x000000, 0);
    lv_obj_set_style_bg_opa(c->wake_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(c->wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->wake_overlay, cancel_wake, LV_EVENT_CLICKED, c);
    if(!ui_ref_backdrop(c->wake_overlay, c->root, 0x000000, 64, 2))
        lv_obj_set_style_bg_opa(c->wake_overlay, 64, 0);

    lv_obj_t *panel = ui_box(c->wake_overlay, 140, 227, 440, 267, 0xfbfaf7, 32);
    ui_ref_shadow(panel, 0x000000, 64, 50, -12, 25);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tile = ui_box(panel, 24, 24, 56, 56, UI_REF_CLAY, 20);
    lv_obj_set_style_bg_opa(tile, 31, 0);
    lv_obj_t *power = ui_ref_icon(tile, "power", 26, UI_REF_CLAY);
    lv_obj_center(power);
    ui_ref_label(panel, "开启台式机？", 22, UI_FONT_SEMIBOLD, UI_REF_INK, 24, 99);
    lv_obj_t *description = ui_ref_label(panel, "小伴会通过网络唤醒你的电脑，大约需要几秒钟。",
                                          16, UI_FONT_REGULAR, UI_REF_MUTED, 24, 141);
    lv_obj_set_width(description, 392);
    lv_obj_t *cancel = ui_ref_button(panel, 24, 187, 190, 56, 28, "desk_wake_cancel");
    ui_centered_label(cancel, "取消", ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(cancel, cancel_wake, LV_EVENT_CLICKED, c);
    lv_obj_t *confirm = ui_button(panel, 226, 187, 190, 56, UI_REF_CLAY, 28);
    ui_centered_label(confirm, "确认开机", ui_ref_font(16, UI_FONT_MEDIUM), 0xffffff);
    lv_obj_add_event_cb(confirm, confirm_wake, LV_EVENT_CLICKED, c);
}

static void desk_action(lv_event_t *event)
{
    desk_binding_t *b = lv_event_get_user_data(event);
    desk_context_t *c = b->context;
    const app_model_t *m = app_state_model(c->app->state);
    int a = b->action;
    if(a <= 6) c->scene = -1;
    if(a == 0) app_state_set_device(c->app->state, 0, !m->devices[0].enabled);
    else if(a <= 3) {
        size_t index = indices[a - 1];
        app_state_set_device(c->app->state, index, !m->devices[index].enabled);
    }
    else if(a <= 6) {
        c->tone = a - 4;
        app_state_set_device(c->app->state, 0, true);
    }
    else if(a == 7) tip_set(c, "桌面一切正常，专心做你的事就好。", 0);
    else if(a == 8) {
        if(c->tip_action == 2) {
            app_state_sleep_computer(c->app->state);
            tip_set(c, "电脑已休眠，需要时点一下就能唤醒。", 0);
        }
        else {
            c->brightness = 80;
            app_state_set_device(c->app->state, 0, true);
            tip_set(c, "亮一点眼睛更舒服，记得每 45 分钟抬头看看远处。", 0);
        }
    }
    else if(a == 9) {
        if(m->computer == APP_COMPUTER_ONLINE) tip_set(c, "电脑正在运行。要让它休眠吗？", 2);
        else if(m->computer == APP_COMPUTER_SLEEP) {
            open_wake_dialog(c);
        }
    }
    else if(a >= 14) {
        c->scene = -1;
        if(a == 14) c->charge = c->charge ? 0 : 1;
        else if(c->charge) c->charge = c->charge == 2 ? 1 : 2;
    }
    else {
        int scene = a - 10;
        static const int brightness[] = {85, 70, 20, 45}, tone[] = {1, 2, 0, 0};
        c->scene = scene;
        c->brightness = brightness[scene];
        c->tone = tone[scene];
        app_state_set_device(c->app->state, 0, scene < 2);
        app_state_set_device(c->app->state, indices[0], scene < 2);
        app_state_set_device(c->app->state, indices[1], false);
        app_state_set_device(c->app->state, indices[2], scene == 0 || scene == 2);
        if(scene < 2) wake_pc(c);
        else app_state_sleep_computer(c->app->state);
        tip_set(c, tips[scene], 0);
    }
    desk_sync(c, app_state_model(c->app->state));
}

static void bind(desk_context_t *c, lv_obj_t *button, int action)
{
    c->bindings[action] = (desk_binding_t){c, action};
    lv_obj_add_event_cb(button, desk_action, LV_EVENT_CLICKED, &c->bindings[action]);
}

static void brightness_changed(lv_event_t *event)
{
    desk_context_t *c = lv_event_get_user_data(event);
    c->brightness = lv_slider_get_value(c->slider);
    if(c->brightness < 5) c->brightness = 5;
    c->scene = -1;
    app_state_set_device(c->app->state, 0, true);
    desk_sync(c, app_state_model(c->app->state));
}

static void desk_update(void *data, const app_model_t *model, uint32_t changes)
{
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_DEVICES)) desk_sync(data, model);
}
static void desk_paused(void *data, bool paused) { ((desk_context_t *)data)->paused = paused; }
static void charge_timer(lv_timer_t *timer)
{
    desk_context_t *c = lv_timer_get_user_data(timer);
    if(c->paused || c->app->content_frozen || c->wake_overlay || c->charge != 2) return;
    lv_obj_set_style_transform_rotation(c->charge_ring, (platform_monotonic_ms() % 3000) * 3600 / 3000, 0);
}
static void desk_destroy(void *data)
{
    desk_context_t *c = data;
    if(c->charge_timer) lv_timer_delete(c->charge_timer);
    free(c);
}

ui_screen_t ui_devices_screen_create(ui_app_t *app)
{
    desk_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app; c->brightness = 45; c->tone = 1; c->scene = -1; c->charge = 2;
    c->previous_pc = app_state_model(app->state)->computer;
    lv_obj_t *root = ui_ref_shell_latest(app, "桌面设备", "MY DESK");
    c->root = root;
    lv_obj_t *badge = ui_box(root, 523, 31, 168, 40, 0xffffff, 20);
    ui_ref_surface(badge, "desk_status");
    lv_obj_t *sun = ui_ref_icon(badge, "sun", 17, UI_REF_CLAY);
    lv_obj_set_pos(sun, 14, 12);
    c->light = ui_ref_label(badge, "", 15, UI_FONT_REGULAR, 0x525252, 39, 11);
    ui_box(badge, 111, 13, 1, 14, 0xe5e5e5, 0);
    ui_ref_label(badge, "26°C", 15, UI_FONT_REGULAR, 0x525252, 120, 11);
    lv_obj_t *tip = ui_box(root, 22, 96, 676, 75, 0xffffff, 26);
    ui_ref_surface(tip, "desk_tip");
    lv_obj_t *orb = ui_box(tip, 18, 15, 44, 44, UI_REF_INK, 22);
    lv_obj_t *spark = ui_ref_icon(orb, "spark", 19, 0xffffff);
    lv_obj_center(spark);
    c->tip = ui_ref_label(tip, "", 17, UI_FONT_REGULAR, UI_REF_INK, 76, 14);
    lv_obj_set_style_text_line_space(c->tip, 3, 0);
    c->dismiss = ui_ref_button(tip, 522, 15, 62, 44, 22, "desk_dismiss");
    ui_centered_label(c->dismiss, "不用", ui_ref_font(15, UI_FONT_REGULAR), 0x525252);
    bind(c, c->dismiss, 7);
    c->accept = ui_button(tip, 592, 15, 66, 44, UI_REF_CLAY, 22);
    ui_centered_label(c->accept, "调亮", ui_ref_font(15, UI_FONT_MEDIUM), 0xffffff);
    bind(c, c->accept, 8);
    c->pc = ui_box(root, 22, 183, 676, 116, 0xffffff, 30);
    ui_ref_surface(c->pc, "desk_pc");
    c->monitor = ui_box(c->pc, 20, 27, 86, 62, 0xf5f5f5, 12);
    lv_obj_set_style_border_width(c->monitor, 3, 0);
    c->stand = ui_box(c->pc, 51, 89, 24, 8, 0xd4d4d4, 3);
    c->pc_en = ui_ref_label(c->pc, "DESKTOP PC", 14, UI_FONT_REGULAR, UI_REF_FAINT, 126, 21);
    c->pc_name = ui_ref_label(c->pc, "我的台式机", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 126, 42);
    c->pc_dot = ui_box(c->pc, 126, 81, 8, 8, 0xd4d4d4, 4);
    c->pc_status = ui_ref_label(c->pc, "", 15, UI_FONT_REGULAR, UI_REF_FAINT, 140, 75);
    c->pc_button = ui_button(c->pc, 520, 20, 136, 76, UI_REF_CLAY, 38);
    ui_ref_shadow(c->pc_button, UI_REF_CLAY, 220, 48, -10, 14);
    lv_obj_t *power_orb = ui_box(c->pc_button, 12, 12, 52, 52, 0xffffff, 26);
    lv_obj_set_style_bg_opa(power_orb, 51, 0);
    lv_obj_t *pc_icon = ui_ref_icon(power_orb, "power", 26, 0xffffff);
    lv_obj_center(pc_icon);
    c->pc_button_label = ui_ref_label(c->pc_button, "开机", 19, UI_FONT_SEMIBOLD, 0xffffff, 74, 27);
    bind(c, c->pc_button, 9);
    lv_obj_t *lamp = ui_box(root, 22, 311, 355, 319, 0xffffff, 26);
    ui_ref_surface(lamp, "desk_lamp");
    lv_obj_set_style_clip_corner(lamp, true, 0);
    c->glow = ui_box(lamp, 0, 0, 355, 320, 0, 0);
    lv_obj_set_style_bg_opa(c->glow, 0, 0);
    ui_ref_label(lamp, "台灯 · Lamp", 15, UI_FONT_REGULAR, UI_REF_FAINT, 18, 18);
    c->value = ui_ref_label(lamp, "45", 34, UI_FONT_SEMIBOLD, UI_REF_INK, 18, 45);
    c->percent = ui_ref_label(lamp, "%", 18, UI_FONT_REGULAR, UI_REF_FAINT, 63, 59);
    c->power = ui_button(lamp, 289, 18, 48, 48, UI_REF_CLAY, 24);
    c->power_icon = ui_ref_icon(c->power, "power", 21, 0xffffff);
    lv_obj_center(c->power_icon);
    bind(c, c->power, 0);
    c->slider = lv_slider_create(lamp);
    lv_obj_remove_style_all(c->slider);
    lv_obj_set_pos(c->slider, 18, 205);
    lv_obj_set_size(c->slider, 319, 48);
    lv_slider_set_range(c->slider, 0, 100);
    lv_obj_set_style_bg_color(c->slider, lv_color_hex(0xececec), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->slider, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(c->slider, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c->slider, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(c->slider, 255, LV_PART_INDICATOR);
    lv_obj_set_style_radius(c->slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_clip_corner(c->slider, true, 0);
    lv_obj_set_style_bg_opa(c->slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(c->slider, brightness_changed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_t *bulb = ui_ref_icon(lamp, "bulb", 20, UI_REF_MUTED);
    lv_obj_set_pos(bulb, 34, 219);
    for(int i = 0; i < 3; ++i) {
        c->tones[i] = ui_button(lamp, 18 + i * 109, 261, 101, 40, 0xe6e6e6, 20);
        ui_box(c->tones[i], 27, 14, 12, 12, colors[i], 6);
        c->tone_labels[i] = ui_ref_label(c->tones[i], tones[i], 14, UI_FONT_REGULAR, 0x525252, 45, 12);
        bind(c, c->tones[i], 4 + i);
        c->devices[i] = ui_button(root, 389 + (i * 319 + 1) / 3, 544, 96, 86, 0xffffff, 20);
        ui_ref_surface(c->devices[i], "desk_compact");
        lv_obj_set_style_border_color(c->devices[i], lv_color_hex(0xf5b6a0), 0);
        c->tiles[i] = ui_box(c->devices[i], 30, 15, 36, 36, 0xffffff, 12);
        c->icons[i] = ui_ref_icon(c->tiles[i], icons[i], 19, UI_REF_FAINT);
        lv_obj_center(c->icons[i]);
        c->names[i] = ui_ref_label(c->devices[i], names[i], 13, UI_FONT_MEDIUM, UI_REF_INK, 0, 56);
        lv_obj_set_width(c->names[i], 96);
        lv_obj_set_style_text_align(c->names[i], LV_TEXT_ALIGN_CENTER, 0);
        c->statuses[i] = ui_ref_label(c->devices[i], "", 13, UI_FONT_REGULAR, UI_REF_FAINT, 0, 0);
        lv_obj_add_flag(c->statuses[i], LV_OBJ_FLAG_HIDDEN);
        bind(c, c->devices[i], i + 1);
    }
    lv_obj_t *charge = ui_box(root, 389, 311, 309, 223, 0xffffff, 24);
    ui_ref_surface(charge, "desk_charge");
    lv_obj_set_style_clip_corner(charge, true, 0);
    c->charge_glow = ui_box(charge, 0, 0, 309, 223, 0, 0);
    lv_obj_set_style_bg_opa(c->charge_glow, 0, 0);
    ui_ref_surface(c->charge_glow, "charge_glow");
    lv_obj_t *pad = ui_button(charge, 16, 68, 88, 88, 0, 44);
    lv_obj_set_style_bg_opa(pad, 0, 0);
    bind(c, pad, 15);
    c->charge_ring = ui_box(pad, 0, 0, 88, 88, 0, 44);
    lv_obj_set_style_transform_pivot_x(c->charge_ring, 44, 0);
    lv_obj_set_style_transform_pivot_y(c->charge_ring, 44, 0);
    lv_obj_set_style_bg_opa(c->charge_ring, 0, 0);
    ui_ref_surface(c->charge_ring, "charge_ring");
    lv_obj_t *surface = ui_box(pad, 0, 0, 88, 88, 0, 44);
    lv_obj_set_style_bg_opa(surface, 0, 0);
    ui_ref_surface(surface, "charge_pad");
    c->charge_phone = ui_box(pad, 27, 15, 34, 58, UI_REF_INK, 10);
    ui_ref_shadow(c->charge_phone, 0, 128, 14, -6, 8);
    ui_box(c->charge_phone, 12, 6, 10, 4, 0x535353, 2);
    lv_obj_t *bolt = ui_ref_icon(c->charge_phone, "bolt", 16, UI_REF_CLAY); lv_obj_center(bolt);
    c->charge_bolt = ui_ref_icon(pad, "bolt", 20, UI_REF_CLAY); lv_obj_center(c->charge_bolt);
    ui_ref_label(charge, "无线充电", 16, UI_FONT_SEMIBOLD, UI_REF_INK, 120, 73);
    ui_ref_label(charge, "Wireless · Qi 15W", 12, UI_FONT_REGULAR, UI_REF_FAINT, 120, 95);
    c->charge_badge = ui_box(charge, 120, 123, 82, 28, 0xfcece6, 14);
    c->charge_dot = ui_box(c->charge_badge, 10, 11, 6, 6, UI_REF_CLAY, 3);
    c->charge_status = ui_ref_label(c->charge_badge, "充电中", 13, UI_FONT_MEDIUM, UI_REF_CLAY, 22, 6);
    c->charge_toggle = ui_button(charge, 249, 98, 44, 26, UI_REF_CLAY, 13);
    c->charge_knob = ui_box(c->charge_toggle, 21, 3, 20, 20, 0xffffff, 10);
    bind(c, c->charge_toggle, 14);
    c->charge_timer = lv_timer_create(charge_timer, 80, c);
    for(int i = 0; i < 4; ++i) {
        c->scenes[i] = ui_ref_button(root, 22 + (i * 343 + 1) / 2, 642, 162, 56, 28, "desk_scene");
        c->scene_icons[i] = ui_ref_icon(c->scenes[i], scene_icons[i], 20, UI_REF_CLAY);
        lv_obj_set_pos(c->scene_icons[i], 51, 18);
        c->scene_labels[i] = ui_ref_label(c->scenes[i], scenes[i], 16, UI_FONT_MEDIUM, 0x404040, 79, 18);
        bind(c, c->scenes[i], i + 10);
    }
    tip_set(c, "天色暗下来了，桌面光线有点不够，帮你把台灯调亮一些？", 1);
    desk_sync(c, app_state_model(app->state));
    return (ui_screen_t){root, c, desk_update, desk_destroy, desk_paused};
}
