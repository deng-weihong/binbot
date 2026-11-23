#include "../ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bluetooth_context bluetooth_context_t;
typedef struct {
    bluetooth_context_t *context;
    char path[BINBOT_BLUETOOTH_PATH_MAX];
    binbot_bluetooth_action_t action;
} bluetooth_binding_t;

struct bluetooth_context {
    ui_app_t *app;
    lv_obj_t *root;
    lv_obj_t *power;
    lv_obj_t *body;
    lv_obj_t *list;
    lv_obj_t *bars[5];
    lv_obj_t *rings[2];
    lv_obj_t *spinners[BINBOT_BLUETOOTH_MAX_DEVICES];
    lv_obj_t *volume;
    lv_obj_t *volume_fill;
    lv_obj_t *volume_value;
    lv_obj_t *volume_icon;
    lv_timer_t *timer;
    bool rendered;
    bool dragging_volume;
    bool volume_pending;
    unsigned volume_percent;
    unsigned phase;
    binbot_bluetooth_snapshot_t snapshot;
    bluetooth_binding_t bindings[BINBOT_BLUETOOTH_MAX_DEVICES * 2 + 1];
};

static void sync(bluetooth_context_t *context);

static void toggle_style(lv_obj_t *toggle, bool on, bool enabled)
{
    lv_obj_set_style_bg_color(toggle, lv_color_hex(on ? UI_REF_CLAY : 0xd6d6d6), 0);
    lv_obj_set_x(lv_obj_get_child(toggle, 0), on ? 29 : 4);
    if(enabled) lv_obj_remove_state(toggle, LV_STATE_DISABLED);
    else lv_obj_add_state(toggle, LV_STATE_DISABLED);
}

static lv_obj_t *toggle(lv_obj_t *parent, int x, int y, bool on)
{
    lv_obj_t *button = ui_button(parent, x, y, 61, 36, on ? UI_REF_CLAY : 0xd6d6d6, 18);
    lv_obj_t *thumb = ui_box(button, on ? 29 : 4, 4, 29, 29, 0xffffff, 15);
    ui_ref_shadow(thumb, 0x000000, 25, 6, 0, 2);
    return button;
}

static void request(bluetooth_context_t *context, binbot_bluetooth_action_t action,
                      const char *device, unsigned value)
{
    if(!binbot_bluetooth_manager_request(context->app->bluetooth, action, device, value))
        ui_app_show_toast(context->app, "蓝牙暂不可用或正在处理中");
}

static void power_clicked(lv_event_t *event)
{
    bluetooth_context_t *context = lv_event_get_user_data(event);
    request(context, BINBOT_BLUETOOTH_POWER, NULL, !context->snapshot.powered);
}

static void discovery_clicked(lv_event_t *event)
{
    bluetooth_context_t *context = lv_event_get_user_data(event);
    request(context, BINBOT_BLUETOOTH_DISCOVERABLE, NULL, !context->snapshot.discoverable);
}

static void device_clicked(lv_event_t *event)
{
    bluetooth_binding_t *binding = lv_event_get_user_data(event);
    request(binding->context, binding->action, binding->path, 0);
}

static void media_request(bluetooth_context_t *context, binbot_bluetooth_action_t action)
{
    if(!context->snapshot.playback_supported) {
        ui_app_show_toast(context->app, "对方设备未提供播放控制"); return;
    }
    request(context, action, NULL, 0);
}

static void playback_clicked(lv_event_t *event)
{
    bluetooth_context_t *context = lv_event_get_user_data(event);
    media_request(context, context->snapshot.playing ? BINBOT_BLUETOOTH_PAUSE : BINBOT_BLUETOOTH_PLAY);
}

static void previous_clicked(lv_event_t *event)
{ media_request(lv_event_get_user_data(event), BINBOT_BLUETOOTH_PREVIOUS); }

static void next_clicked(lv_event_t *event)
{ media_request(lv_event_get_user_data(event), BINBOT_BLUETOOTH_NEXT); }

static void volume_draw(bluetooth_context_t *context)
{
    unsigned value = context->volume_percent;
    lv_obj_set_width(context->volume_fill, (int)(636 * (value < 6 ? 6 : value) / 100));
    char text[16]; snprintf(text, sizeof(text), "%u%%", value);
    lv_label_set_text(context->volume_value, text);
    lv_obj_set_style_image_recolor(lv_obj_get_child(context->volume_icon, 0),
                                    lv_color_hex(value > 12 ? UI_REF_INK : 0xbababa), 0);
}

static void volume_event(lv_event_t *event)
{
    bluetooth_context_t *context = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_PRESS_LOST) {
        context->dragging_volume = false; context->rendered = false; return;
    }
    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(event);
        if(!indev) return;
        context->dragging_volume = true;
        lv_point_t point; lv_area_t area;
        lv_indev_get_point(indev, &point); lv_obj_get_coords(context->volume, &area);
        int value = ((point.x - area.x1) * 100 + 318) / 636;
        context->volume_percent = (unsigned)(value < 0 ? 0 : value > 100 ? 100 : value);
        volume_draw(context);
    }
    if(code == LV_EVENT_RELEASED && context->dragging_volume) {
        context->dragging_volume = false;
        if(context->snapshot.volume_supported) {
            context->volume_pending = binbot_bluetooth_manager_request(context->app->bluetooth,
                                          BINBOT_BLUETOOTH_VOLUME, NULL, context->volume_percent);
            if(!context->volume_pending) {
                ui_app_show_toast(context->app, "蓝牙正在处理中"); context->rendered = false;
            }
        }
        else if(context->app->audio_client &&
                binbot_audio_client_set_volume(context->app->audio_client, (uint8_t)context->volume_percent))
            app_state_set_volume(context->app->state, (uint8_t)context->volume_percent);
        else {
            ui_app_show_toast(context->app, "音量调节暂不可用"); context->rendered = false;
        }
    }
}

static lv_obj_t *fitted_label(lv_obj_t *parent, const char *text, unsigned size,
                               ui_font_weight_t weight, unsigned color, int x, int y, int width)
{
    lv_obj_t *label = ui_ref_label(parent, text, size, weight, color, x, y);
    lv_obj_set_size(label, width, lv_font_get_line_height(ui_ref_font(size, weight)));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static void render_list(bluetooth_context_t *context, bool connected)
{
    const binbot_bluetooth_snapshot_t *snapshot = &context->snapshot;
    int top = connected ? 361 : 288, height = 698 - top;
    lv_obj_t *card = ui_box(context->body, 22, top, 676, height, 0xffffff, 24);
    ui_ref_surface(card, connected ? "bluetooth_list_connected" : "bluetooth_list");
    ui_ref_label(card, "已配对设备 · Paired", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 18);
    context->list = ui_box(card, 8, 43, 660, height - 51, 0, 0);
    lv_obj_set_style_bg_opa(context->list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(context->list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(context->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->list, LV_SCROLLBAR_MODE_OFF);
    if(!snapshot->device_count) {
        lv_obj_t *empty = ui_ref_label(context->list, "还没有配对过的设备", 15,
                                        UI_FONT_REGULAR, UI_REF_FAINT, 0, 24);
        lv_obj_set_width(empty, 660); lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    for(size_t i = 0; i < snapshot->device_count; ++i) {
        const binbot_bluetooth_device_t *device = &snapshot->devices[i];
        bool joining = snapshot->busy && !strcmp(snapshot->pending_device, device->path);
        lv_obj_t *row = ui_box(context->list, 0, (int)i * 64, 660, 64, 0, 16);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *icon_box = ui_box(row, 12, 12, 40, 40, device->connected ? UI_REF_CLAY : 0xe6e6e6, 12);
        if(!device->connected) ui_ref_surface(icon_box, "soft40");
        lv_obj_t *icon = ui_ref_icon(icon_box, device->computer ? "monitor" : "bt", 19,
                                     device->connected ? 0xffffff : UI_REF_MUTED);
        lv_obj_center(icon);
        fitted_label(row, device->name, 17, UI_FONT_MEDIUM, UI_REF_INK, 64, 10, 440);
        ui_ref_label(row, joining ? "正在连接…" : device->connected ? "已连接 · 播放音频" : "已配对",
                       13, UI_FONT_REGULAR, joining ? UI_REF_CLAY : UI_REF_FAINT, 64, 34);
        bluetooth_binding_t *binding = &context->bindings[i * 2];
        *binding = (bluetooth_binding_t){.context = context, .action = BINBOT_BLUETOOTH_CONNECT};
        snprintf(binding->path, sizeof(binding->path), "%s", device->path);
        if(joining) {
            lv_obj_t *spinner = ui_ref_ring(row, 556, 20, 24, 3);
            context->spinners[i] = spinner;
            lv_obj_set_style_arc_opa(spinner, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_arc_set_value(spinner, 80);
        }
        else if(device->connected)
            ui_ref_label(row, "使用中", 14, UI_FONT_MEDIUM, UI_REF_CLAY, 538, 23);
        else {
            lv_obj_t *connect = ui_ref_button(row, 526, 10, 66, 44, 22, "bluetooth_connect");
            ui_centered_label(connect, "连接", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
            lv_obj_add_event_cb(connect, device_clicked, LV_EVENT_CLICKED, binding);
            if(snapshot->busy) lv_obj_add_state(connect, LV_STATE_DISABLED);
        }
        binding[1] = *binding; binding[1].action = BINBOT_BLUETOOTH_REMOVE;
        lv_obj_t *remove = ui_button(row, 604, 10, 44, 44, 0, 22);
        lv_obj_set_style_bg_opa(remove, LV_OPA_TRANSP, 0);
        icon = ui_ref_icon(remove, "x", 18, 0xd4d4d4); lv_obj_center(icon);
        lv_obj_add_event_cb(remove, device_clicked, LV_EVENT_CLICKED, &binding[1]);
        if(snapshot->busy) lv_obj_add_state(remove, LV_STATE_DISABLED);
    }
}

static void render(bluetooth_context_t *context)
{
    const binbot_bluetooth_snapshot_t *snapshot = &context->snapshot;
    int scroll = context->list ? lv_obj_get_scroll_y(context->list) : 0;
    lv_obj_clean(context->body); context->list = NULL;
    memset(context->bars, 0, sizeof(context->bars)); memset(context->rings, 0, sizeof(context->rings));
    memset(context->spinners, 0, sizeof(context->spinners));
    context->volume = NULL;
    toggle_style(context->power, snapshot->powered, snapshot->available && !snapshot->busy);
    if(!snapshot->ready || !snapshot->available || !snapshot->powered) {
        lv_obj_t *circle = ui_box(context->body, 312, 291, 96, 96, 0xe6e6e6, 48);
        ui_ref_surface(circle, "bluetooth_off");
        lv_obj_t *icon = ui_ref_icon(circle, "bt", 40, UI_REF_FAINT); lv_obj_center(icon);
        lv_obj_t *text = ui_ref_label(context->body, !snapshot->ready ? "正在读取蓝牙…" :
                                        !snapshot->available ? "蓝牙暂不可用" : "蓝牙已关闭",
                                        20, UI_FONT_SEMIBOLD, UI_REF_INK, 22, 406);
        lv_obj_set_width(text, 676); lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        text = ui_ref_label(context->body, snapshot->available ?
                             "打开后，小伴可以作为蓝牙音箱播放手机、电脑里的声音" :
                             snapshot->ready ? "请检查设备蓝牙服务" : "", 15,
                             UI_FONT_REGULAR, UI_REF_FAINT, 22, 440);
        lv_obj_set_width(text, 676); lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    const binbot_bluetooth_device_t *current = NULL;
    for(size_t i = 0; i < snapshot->device_count; ++i)
        if(snapshot->devices[i].connected) { current = &snapshot->devices[i]; break; }
    lv_obj_t *card = ui_box(context->body, 22, 96, 676, current ? 177 : 104,
                             current ? UI_REF_INK : 0xffffff, 30);
    ui_ref_surface(card, current ? "bluetooth_play" : "bluetooth_wait");
    if(current) {
        lv_obj_t *visual = ui_box(card, 20, 24, 64, 64, UI_REF_CLAY, 20);
        if(snapshot->playing) {
            for(int i = 0; i < 5; ++i) context->bars[i] = ui_box(visual, 14 + i * 8, 18, 4, 28, 0xffffff, 2);
        }
        else { lv_obj_t *icon = ui_ref_icon(visual, "speaker", 28, 0xffffff); lv_obj_center(icon); }
        ui_ref_label(card, snapshot->playing ? "正在播放 · A2DP" : "已暂停 · A2DP", 14,
                       UI_FONT_REGULAR, 0x8d8d8d, 100, 23);
        fitted_label(card, current->name, 22, UI_FONT_SEMIBOLD, 0xffffff, 100, 42, 294);
        char detail[400];
        if(snapshot->title[0]) snprintf(detail, sizeof(detail), "%s%s%s", snapshot->title,
                                         snapshot->artist[0] ? " · " : "", snapshot->artist);
        else snprintf(detail, sizeof(detail), "%s", snapshot->artist[0] ? snapshot->artist : "未提供曲目信息");
        fitted_label(card, detail, 15, UI_FONT_REGULAR, 0xc6c6c6, 100, 72, 294);
        lv_obj_t *previous = ui_button(card, 410, 32, 48, 48, 0xffffff, 24);
        lv_obj_set_style_bg_opa(previous, 26, 0);
        lv_obj_t *icon = ui_ref_icon(previous, "prev", 20, 0xffffff); lv_obj_center(icon);
        lv_obj_add_event_cb(previous, previous_clicked, LV_EVENT_CLICKED, context);
        lv_obj_t *play = ui_button(card, 464, 28, 56, 56, 0xffffff, 28);
        icon = ui_ref_icon(play, snapshot->playing ? "pause" : "play", 24, UI_REF_INK); lv_obj_center(icon);
        lv_obj_add_event_cb(play, playback_clicked, LV_EVENT_CLICKED, context);
        lv_obj_t *next = ui_button(card, 526, 32, 48, 48, 0xffffff, 24);
        lv_obj_set_style_bg_opa(next, 26, 0);
        icon = ui_ref_icon(next, "next", 20, 0xffffff); lv_obj_center(icon);
        lv_obj_add_event_cb(next, next_clicked, LV_EVENT_CLICKED, context);
        lv_obj_t *disconnect = ui_button(card, 590, 28, 66, 56, 0xffffff, 28);
        lv_obj_set_style_bg_opa(disconnect, 26, 0);
        ui_centered_label(disconnect, "断开", ui_ref_font(15, UI_FONT_REGULAR), 0xffffff);
        bluetooth_binding_t *binding = &context->bindings[BINBOT_BLUETOOTH_MAX_DEVICES * 2];
        *binding = (bluetooth_binding_t){.context = context, .action = BINBOT_BLUETOOTH_DISCONNECT};
        snprintf(binding->path, sizeof(binding->path), "%s", current->path);
        lv_obj_add_event_cb(disconnect, device_clicked, LV_EVENT_CLICKED, binding);
        if(snapshot->busy) {
            lv_obj_add_state(previous, LV_STATE_DISABLED); lv_obj_add_state(next, LV_STATE_DISABLED);
            lv_obj_add_state(play, LV_STATE_DISABLED); lv_obj_add_state(disconnect, LV_STATE_DISABLED);
        }
        context->volume = ui_box(card, 20, 109, 636, 48, 0xffffff, 18);
        lv_obj_set_style_bg_opa(context->volume, 26, 0);
        lv_obj_add_flag(context->volume, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        context->volume_fill = ui_box(context->volume, 0, 0, 350, 48, 0xffffff, 18);
        lv_obj_set_style_bg_opa(context->volume_fill, 230, 0);
        context->volume_icon = ui_ref_icon(context->volume, "speaker", 20, UI_REF_INK);
        lv_obj_set_pos(context->volume_icon, 16, 14);
        context->volume_value = ui_ref_label(context->volume, "", 15, UI_FONT_SEMIBOLD, 0xdddddd, 550, 13);
        lv_obj_set_width(context->volume_value, 70);
        lv_obj_set_style_text_align(context->volume_value, LV_TEXT_ALIGN_RIGHT, 0);
        if(!context->volume_pending)
            context->volume_percent = snapshot->volume_supported ? snapshot->volume : app_state_model(context->app->state)->volume;
        volume_draw(context);
        lv_obj_add_event_cb(context->volume, volume_event, LV_EVENT_PRESSED, context);
        lv_obj_add_event_cb(context->volume, volume_event, LV_EVENT_PRESSING, context);
        lv_obj_add_event_cb(context->volume, volume_event, LV_EVENT_RELEASED, context);
        lv_obj_add_event_cb(context->volume, volume_event, LV_EVENT_PRESS_LOST, context);
    }
    else {
        for(unsigned i = 0; i < 2; ++i) {
            context->rings[i] = ui_box(card, 20, 20, 64, 64, UI_REF_CLAY, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_opa(context->rings[i], snapshot->discoverable ? 32 : 0, 0);
        }
        lv_obj_t *circle = ui_box(card, 24, 24, 56, 56, UI_REF_CLAY, 28);
        lv_obj_t *icon = ui_ref_icon(circle, "bt", 26, 0xffffff); lv_obj_center(icon);
        ui_ref_label(card, "等待连接", 20, UI_FONT_SEMIBOLD, UI_REF_INK, 100, 28);
        char hint[256]; snprintf(hint, sizeof(hint), "在手机蓝牙中搜索「%s」即可连接播放", snapshot->name);
        fitted_label(card, hint, 15, UI_FONT_REGULAR, UI_REF_MUTED, 100, 57, 556);
    }
    lv_obj_t *discovery = ui_box(context->body, 22, current ? 285 : 212, 676, 64, 0xffffff, 24);
    ui_ref_surface(discovery, "settings_about_modules");
    ui_ref_label(discovery, "允许被发现", 17, UI_FONT_MEDIUM, UI_REF_INK, 18, 8);
    char name[160]; snprintf(name, sizeof(name), "设备名称：%s", snapshot->name);
    fitted_label(discovery, name, 14, UI_FONT_REGULAR, UI_REF_FAINT, 18, 35, 550);
    lv_obj_t *discover = toggle(discovery, 597, 14, snapshot->discoverable);
    if(snapshot->busy) lv_obj_add_state(discover, LV_STATE_DISABLED);
    lv_obj_add_event_cb(discover, discovery_clicked, LV_EVENT_CLICKED, context);
    render_list(context, current != NULL);
    lv_obj_update_layout(context->root);
    if(scroll) lv_obj_scroll_to_y(context->list, scroll, LV_ANIM_OFF);
}

static void sync(bluetooth_context_t *context)
{
    if(context->app->content_frozen || context->dragging_volume) return;
    binbot_bluetooth_snapshot_t snapshot;
    if(!binbot_bluetooth_manager_snapshot(context->app->bluetooth, &snapshot)) return;
    if(context->rendered && !memcmp(&context->snapshot, &snapshot, sizeof(snapshot))) return;
    if(!snapshot.busy) context->volume_pending = false;
    if(snapshot.error[0] && strcmp(snapshot.error, context->snapshot.error))
        ui_app_show_toast(context->app, snapshot.error);
    context->snapshot = snapshot; context->rendered = true;
    render(context);
}

static void timer_cb(lv_timer_t *timer)
{
    bluetooth_context_t *context = lv_timer_get_user_data(timer);
    if(context->app->content_frozen) return;
    sync(context);
    context->phase += 80;
    for(unsigned i = 0; i < BINBOT_BLUETOOTH_MAX_DEVICES; ++i)
        if(context->spinners[i]) lv_arc_set_rotation(context->spinners[i], (context->phase / 3) % 360);
    for(unsigned i = 0; i < 5; ++i) if(context->bars[i]) {
        double wave = (1 + cos((context->phase - i * 120.0) * 6.2831853 / 900)) / 2;
        int height = (int)(8.4 + wave * 19.6);
        lv_obj_set_height(context->bars[i], height); lv_obj_set_y(context->bars[i], (64 - height) / 2);
    }
    for(unsigned i = 0; i < 2; ++i) if(context->rings[i] && context->snapshot.discoverable) {
        unsigned phase = (context->phase + i * 700) % 1400;
        int size = 64 + (int)(58 * phase / 1400);
        lv_obj_set_size(context->rings[i], size, size);
        lv_obj_set_pos(context->rings[i], 52 - size / 2, 52 - size / 2);
        lv_obj_set_style_bg_opa(context->rings[i], 32 * (1400 - phase) / 1400, 0);
    }
}

static void set_paused(void *data, bool paused)
{
    bluetooth_context_t *context = data;
    if(paused) lv_timer_pause(context->timer);
    else { lv_timer_resume(context->timer); sync(context); }
}

static void destroy(void *data)
{
    bluetooth_context_t *context = data;
    lv_timer_delete(context->timer); free(context);
}

ui_screen_t ui_bluetooth_screen_create(ui_app_t *app)
{
    bluetooth_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->root = ui_ref_shell_latest(app, "蓝牙音箱", "BLUETOOTH · A2DP SINK");
    context->power = toggle(context->root, 630, 33, false);
    lv_obj_add_event_cb(context->power, power_clicked, LV_EVENT_CLICKED, context);
    context->body = ui_box(context->root, 0, 0, 720, 720, 0, 0);
    lv_obj_set_style_bg_opa(context->body, LV_OPA_TRANSP, 0);
    context->timer = lv_timer_create(timer_cb, 80, context);
    sync(context);
    return (ui_screen_t){context->root, context, NULL, destroy, set_paused};
}
