#include "../ui_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct gesture_context gesture_context_t;
typedef struct { gesture_context_t *context; int command; } gesture_binding_t;
struct gesture_context {
    ui_app_t *app;
    app_gesture_id_t selected;
    lv_obj_t *root, *body, *master, *toggle, *confirm, *title, *description, *glyph;
    lv_obj_t *rows[APP_GESTURE_COUNT], *row_names[APP_GESTURE_COUNT];
    lv_obj_t *row_actions[APP_GESTURE_COUNT], *row_tiles[APP_GESTURE_COUNT];
    lv_obj_t *row_icons[APP_GESTURE_COUNT], *dots[APP_GESTURE_COUNT];
    lv_obj_t *action, *action_name, *action_icon, *try_button;
    lv_obj_t *sens[3], *sens_labels[3], *sheet;
    gesture_binding_t bindings[64];
};

static const char *instructions[] = {
    "拇指与食指捏成圆圈，其余三指伸直", "握拳并竖起大拇指",
    "五指张开，掌心朝向屏幕停留 1 秒", "伸出食指和中指，比出 V 字",
    "五指收拢握拳，停留 1 秒",
};

static void sync(gesture_context_t *c);
static void clicked(lv_event_t *event);

static void bind(gesture_context_t *c, lv_obj_t *object, int command)
{
    c->bindings[command] = (gesture_binding_t){c, command};
    lv_obj_add_event_cb(object, clicked, LV_EVENT_CLICKED, &c->bindings[command]);
}

static lv_obj_t *toggle(gesture_context_t *c, lv_obj_t *parent, int x, int y, int command)
{
    lv_obj_t *button = ui_button(parent, x, y, 61, 36, UI_REF_CLAY, 18);
    ui_box(button, 29, 4, 29, 29, 0xffffff, 15);
    bind(c, button, command);
    return button;
}

static void toggle_sync(lv_obj_t *button, bool on)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(on ? UI_REF_CLAY : 0xd6d6d6), 0);
    lv_obj_set_x(lv_obj_get_child(button, 0), on ? 29 : 4);
    lv_obj_set_state(button, LV_STATE_CHECKED, on);
}

static void close_sheet(gesture_context_t *c)
{
    if(c->sheet) lv_obj_delete_async(c->sheet);
    c->sheet = NULL;
}

static void open_sheet(gesture_context_t *c)
{
    if(c->sheet) return;
    const app_gesture_settings_t *s = &app_state_model(c->app->state)->gestures;
    lv_obj_t *overlay = ui_box(c->root, 0, 0, 720, 720, 0, 0);
    c->sheet = overlay;
    lv_obj_set_style_bg_opa(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    bind(c, overlay, 60);
    if(!ui_ref_backdrop(overlay, c->root, 0, 64, 2)) lv_obj_set_style_bg_opa(overlay, 64, 0);
    lv_obj_t *panel = ui_box(overlay, 80, 177, 560, 366, 0xfbfaf7, 32);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    ui_ref_shadow(panel, 0, 64, 50, -12, 25);
    lv_obj_t *tile = ui_box(panel, 24, 24, 44, 44, 0xfcece6, 14);
    lv_obj_t *icon = ui_ref_icon(tile, ui_gesture_icon(c->selected), 24, UI_REF_CLAY); lv_obj_center(icon);
    char text[80]; snprintf(text, sizeof(text), "「%s」执行什么？", ui_gesture_name(c->selected));
    ui_ref_label(panel, text, 22, UI_FONT_SEMIBOLD, UI_REF_INK, 80, 32);
    for(int i = 0; i < APP_GESTURE_ACTION_COUNT; ++i) {
        bool selected = s->actions[c->selected] == i;
        lv_obj_t *button = ui_button(panel, 24 + (i % 3) * 174, 88 + (i / 3) * 66,
                                       164, 56, selected ? UI_REF_INK : 0xe6e6e6, 18);
        if(!selected) ui_ref_surface(button, "gesture_action");
        icon = ui_ref_icon(button, ui_gesture_action_icon(i), 18, selected ? 0xffffff : UI_REF_INK);
        lv_obj_set_pos(icon, 14, 19);
        lv_obj_t *label = ui_ref_label(button, ui_gesture_action_name(i), 16, UI_FONT_MEDIUM,
                                         selected ? 0xffffff : UI_REF_INK, 42, 17);
        lv_obj_set_width(label, 112); lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        bind(c, button, 40 + i);
    }
}

static void back(lv_event_t *event)
{
    ui_app_go_back(lv_event_get_user_data(event));
}

static void clicked(lv_event_t *event)
{
    gesture_binding_t *binding = lv_event_get_user_data(event);
    gesture_context_t *c = binding->context;
    int command = binding->command;
    app_gesture_settings_t s = app_state_model(c->app->state)->gestures;
    if(command == 60) { close_sheet(c); return; }
    if(command == 10) s.enabled = !s.enabled;
    else {
        if(!s.enabled) return;
        if(command < APP_GESTURE_COUNT) { c->selected = command; sync(c); return; }
        if(command == 11) s.enabled_mask ^= (uint8_t)(1U << c->selected);
        else if(command == 12) s.confirm_sensitive = !s.confirm_sensitive;
        else if(command >= 20 && command <= 22) s.sensitivity = command - 20;
        else if(command == 30) {
            if(s.enabled_mask & (1U << c->selected)) open_sheet(c);
            return;
        }
        else if(command == 31) { ui_gesture_try(c->app, c->selected); return; }
        else if(command >= 40 && command < 40 + APP_GESTURE_ACTION_COUNT) {
            s.actions[c->selected] = command - 40;
            close_sheet(c);
        }
        else return;
    }
    app_state_set_gestures(c->app->state, &s);
}

static void sync(gesture_context_t *c)
{
    const app_gesture_settings_t *s = &app_state_model(c->app->state)->gestures;
    bool on = (s->enabled_mask & (1U << c->selected)) != 0;
    toggle_sync(c->master, s->enabled);
    toggle_sync(c->toggle, on);
    toggle_sync(c->confirm, s->confirm_sensitive);
    lv_obj_set_style_opa(c->body, s->enabled ? 255 : 115, 0);
    for(int i = 0; i < APP_GESTURE_COUNT; ++i) {
        bool selected = (int)c->selected == i;
        bool enabled = (s->enabled_mask & (1U << i)) != 0;
        lv_obj_set_style_bg_color(c->rows[i], lv_color_hex(UI_REF_INK), 0);
        lv_obj_set_style_bg_opa(c->rows[i], selected ? 255 : 0, 0);
        lv_obj_set_style_text_color(c->row_names[i], lv_color_hex(selected ? 0xffffff : UI_REF_INK), 0);
        lv_obj_set_style_text_color(c->row_actions[i], lv_color_hex(selected ? 0xa3a3a3 : UI_REF_FAINT), 0);
        lv_label_set_text(c->row_actions[i], enabled ? ui_gesture_action_name(s->actions[i]) : "未启用");
        lv_obj_set_style_bg_color(c->row_tiles[i], lv_color_hex(selected ? 0x3d3d3d : enabled ? 0xfcece6 : 0xe8e8e8), 0);
        lv_obj_set_style_image_recolor(lv_obj_get_child(c->row_icons[i], 0),
                                        lv_color_hex(selected ? 0xffffff : enabled ? UI_REF_CLAY : UI_REF_FAINT), 0);
        lv_obj_set_style_bg_color(c->dots[i], lv_color_hex(enabled ? 0x10b981 : selected ? 0x535353 : 0xd4d4d4), 0);
    }
    lv_label_set_text(c->title, ui_gesture_name(c->selected));
    lv_label_set_text(c->description, instructions[c->selected]);
    ui_ref_icon_set_name(c->glyph, ui_gesture_icon(c->selected));
    ui_ref_icon_set_name(c->action_icon, ui_gesture_action_icon(s->actions[c->selected]));
    lv_label_set_text(c->action_name, ui_gesture_action_name(s->actions[c->selected]));
    lv_obj_set_style_opa(c->action, on ? 255 : 102, 0);
    lv_obj_set_style_opa(c->try_button, on ? 255 : 102, 0);
    for(int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_opa(c->sens[i], s->sensitivity == i ? 255 : 0, 0);
        lv_obj_set_style_text_color(c->sens_labels[i], lv_color_hex(s->sensitivity == i ? UI_REF_INK : UI_REF_MUTED), 0);
    }
}

static void update(void *data, const app_model_t *model, uint32_t changes)
{
    (void)model;
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_GESTURES)) sync(data);
}

static void destroy(void *data) { free(data); }

ui_screen_t ui_gestures_screen_create(ui_app_t *app)
{
    gesture_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app;
    c->root = ui_ref_page(app);
    lv_obj_t *button = ui_ref_button(c->root, 29, 26, 50, 50, 25, "circle50");
    lv_obj_t *icon = ui_ref_icon(button, "back", 25, UI_REF_INK); lv_obj_center(icon);
    lv_obj_add_event_cb(button, back, LV_EVENT_CLICKED, app);
    ui_ref_label(c->root, "GESTURES", 15, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27);
    ui_ref_label(c->root, "手势设置", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 48);
    lv_obj_t *header = ui_box(c->root, 532, 27, 159, 48, 0xffffff, 24);
    ui_ref_surface(header, "gesture_header");
    ui_ref_shadow(header, 0, 31, 30, -12, 10);
    ui_ref_label(header, "手势识别", 15, UI_FONT_MEDIUM, UI_REF_INK, 18, 14);
    c->master = toggle(c, header, 90, 6, 10);
    c->body = ui_box(c->root, 0, 96, 720, 602, 0, 0);
    lv_obj_set_style_bg_opa(c->body, 0, 0);
    lv_obj_t *list = ui_box(c->body, 22, 0, 300, 602, 0xffffff, 28);
    ui_ref_surface(list, "gesture_list"); ui_ref_shadow(list, 0, 31, 30, -12, 10);
    for(int i = 0; i < APP_GESTURE_COUNT; ++i) {
        c->rows[i] = ui_button(list, 8, 8 + i * 64, 284, 64, UI_REF_INK, 20);
        c->row_tiles[i] = ui_box(c->rows[i], 12, 10, 44, 44, 0xfcece6, 14);
        c->row_icons[i] = ui_ref_icon(c->row_tiles[i], ui_gesture_icon(i), 24, UI_REF_CLAY);
        lv_obj_center(c->row_icons[i]);
        c->row_names[i] = ui_ref_label(c->rows[i], ui_gesture_name(i), 16, UI_FONT_SEMIBOLD, UI_REF_INK, 68, 10);
        c->row_actions[i] = ui_ref_label(c->rows[i], "", 14, UI_FONT_REGULAR, UI_REF_FAINT, 68, 34);
        lv_obj_set_width(c->row_actions[i], 172); lv_label_set_long_mode(c->row_actions[i], LV_LABEL_LONG_DOT);
        c->dots[i] = ui_box(c->rows[i], 264, 28, 8, 8, 0x10b981, 4);
        bind(c, c->rows[i], i);
    }
    lv_obj_t *detail = ui_box(c->body, 336, 0, 362, 188, 0xffffff, 28);
    ui_ref_surface(detail, "gesture_detail"); ui_ref_shadow(detail, 0, 31, 30, -12, 10);
    lv_obj_t *tile = ui_box(detail, 20, 20, 76, 76, 0xfcece6, 24);
    c->glyph = ui_ref_icon(tile, "gesture_ok", 44, UI_REF_CLAY); lv_obj_center(c->glyph);
    c->title = ui_ref_label(detail, "", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 112, 20);
    lv_obj_set_width(c->title, 151); lv_label_set_long_mode(c->title, LV_LABEL_LONG_DOT);
    c->description = ui_ref_label(detail, "", 15, UI_FONT_REGULAR, UI_REF_MUTED, 112, 54);
    lv_obj_set_width(c->description, 151); lv_label_set_long_mode(c->description, LV_LABEL_LONG_WRAP);
    c->toggle = toggle(c, detail, 281, 20, 11);
    c->action = ui_ref_button(detail, 20, 112, 224, 56, 18, "gesture_select");
    ui_ref_label(c->action, "执行", 15, UI_FONT_REGULAR, UI_REF_FAINT, 16, 17);
    tile = ui_box(c->action, 59, 12, 32, 32, 0xffffff, 16);
    c->action_icon = ui_ref_icon(tile, "monitor", 17, UI_REF_INK); lv_obj_center(c->action_icon);
    c->action_name = ui_ref_label(c->action, "", 17, UI_FONT_SEMIBOLD, UI_REF_INK, 102, 16);
    lv_obj_set_width(c->action_name, 88); lv_label_set_long_mode(c->action_name, LV_LABEL_LONG_DOT);
    icon = ui_ref_icon(c->action, "back", 18, UI_REF_FAINT); lv_obj_set_pos(icon, 194, 19);
    lv_image_set_rotation(lv_obj_get_child(icon, 0), 1800);
    bind(c, c->action, 30);
    c->try_button = ui_button(detail, 254, 112, 88, 56, UI_REF_CLAY, 18);
    ui_centered_label(c->try_button, "试一试", ui_ref_font(16, UI_FONT_MEDIUM), 0xffffff);
    bind(c, c->try_button, 31);
    lv_obj_t *options = ui_box(c->body, 336, 200, 362, 188, 0xffffff, 28);
    ui_ref_surface(options, "gesture_options"); ui_ref_shadow(options, 0, 31, 30, -12, 10);
    ui_ref_label(options, "识别灵敏度", 17, UI_FONT_MEDIUM, UI_REF_INK, 20, 20);
    lv_obj_t *hint = ui_ref_label(options, "越高越容易触发，也更易误触", 14, UI_FONT_REGULAR, UI_REF_FAINT, 20, 46);
    lv_obj_set_width(hint, 162); lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_t *segments = ui_box(options, 182, 31, 160, 44, 0xe6e6e6, 22);
    const char *levels[] = {"低", "中", "高"};
    for(int i = 0; i < 3; ++i) {
        c->sens[i] = ui_button(segments, 4 + i * 52, 4, 48, 36, 0xffffff, 18);
        c->sens_labels[i] = ui_centered_label(c->sens[i], levels[i], ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_MUTED);
        bind(c, c->sens[i], 20 + i);
    }
    ui_box(options, 20, 104, 322, 1, 0xf3f3f3, 0);
    ui_ref_label(options, "敏感操作需确认", 17, UI_FONT_MEDIUM, UI_REF_INK, 20, 120);
    ui_ref_label(options, "打开电脑、电脑休眠前先询问", 14, UI_FONT_REGULAR, UI_REF_FAINT, 20, 146);
    c->confirm = toggle(c, options, 281, 125, 12);
    icon = ui_ref_icon(c->body, "camera", 16, UI_REF_FAINT); lv_obj_set_pos(icon, 344, 584);
    hint = ui_ref_label(c->body, "手势通过前置摄像头在本机识别，画面不会上传", 14, UI_FONT_REGULAR, UI_REF_FAINT, 368, 580);
    lv_obj_set_width(hint, 322); lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    sync(c);
    return (ui_screen_t){c->root, c, update, destroy, NULL};
}
